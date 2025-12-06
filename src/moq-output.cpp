#include <obs.hpp>

#include "moq-output.h"
#include "util/util_uint64.h"

extern "C" {
#include "hang.h"
}

MoQOutput::MoQOutput(obs_data_t *, obs_output_t *output)
	: output(output),
	  server_url(),
	  path(),
	  total_bytes_sent(0),
	  connect_time_ms(0),
	  session(-1),
	  video(-1),
	  audio(-1),
	  broadcast(hang_broadcast_create())
{
}

MoQOutput::~MoQOutput()
{
	hang_broadcast_close(broadcast);

	Stop();
}

bool MoQOutput::Start()
{
	obs_service_t *service = obs_output_get_service(output);
	if (!service) {
		LOG_ERROR("Failed to get service from output");
		obs_output_signal_stop(output, OBS_OUTPUT_ERROR);
		return false;
	}

	if (!obs_output_can_begin_data_capture(output, 0)) {
		LOG_ERROR("Cannot begin data capture");
		return false;
	}

	if (!obs_output_initialize_encoders(output, 0)) {
		LOG_ERROR("Failed to initialize encoders");
		return false;
	}

	server_url = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
	if (server_url.empty()) {
		LOG_ERROR("Server URL is empty");
		obs_output_signal_stop(output, OBS_OUTPUT_BAD_PATH);
		return false;
	}

	path = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_STREAM_KEY);

	const obs_encoder_t *encoder = obs_output_get_video_encoder2(output, 0);

	if (!encoder) {
		LOG_ERROR("Failed to get video encoder");
		return false;
	}

	LOG_INFO("Connecting to MoQ server: %s", server_url.c_str());

	connect_start = std::chrono::steady_clock::now();

	// Create a callback to log when the session is connected or closed
	auto session_connect_callback = [](void *user_data, int error_code) {
		auto self = static_cast<MoQOutput*>(user_data);

		if (error_code == 0) {
			auto elapsed = std::chrono::steady_clock::now() - self->connect_start;
			self->connect_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
			LOG_INFO("MoQ session established (%d ms): %s", self->connect_time_ms, self->server_url.c_str());
		} else {
			LOG_INFO("MoQ session closed (%d): %s", error_code, self->server_url.c_str());
		}
	};

	// Start establishing a session with the MoQ server
	// NOTE: You could publish the same broadcasts to multiple sessions if you want (redundant ingest).
	session = hang_session_connect(server_url.c_str(), session_connect_callback, this);
	if (session < 0) {
		LOG_ERROR("Failed to initialize MoQ server: %d", session);
		return false;
	}

	LOG_INFO("Publishing broadcast: %s", path.c_str());

	// Publish the one broadcast to the session.
	// NOTE: You could publish multiple broadcasts to the same session if you want (multi ingest).
	// TODO: There is currently no unpublish function.
	auto result = hang_broadcast_publish(broadcast, session, path.c_str());
	if (result < 0) {
		LOG_ERROR("Failed to publish broadcast to session: %d", result);
		return false;
	}

	obs_output_begin_data_capture(output, 0);

	return true;
}

void MoQOutput::Stop(bool signal)
{
	// Close the session
	hang_session_close(session);
	hang_track_close(video);
	hang_track_close(audio);

	if (signal) {
		obs_output_signal_stop(output, OBS_OUTPUT_SUCCESS);
	}

	return;
}

void MoQOutput::Data(struct encoder_packet *packet)
{
	if (!packet) {
		Stop(false);
		obs_output_signal_stop(output, OBS_OUTPUT_ENCODE_ERROR);
		return;
	}

	if (packet->type == OBS_ENCODER_AUDIO) {
		AudioData(packet);
	} else if (packet->type == OBS_ENCODER_VIDEO) {
		VideoData(packet);
	}
}

void MoQOutput::AudioData(struct encoder_packet *packet)
{
	if (audio < 0) {
		AudioInit();
	}

	auto result = hang_track_write(audio, packet->data, packet->size, packet->pts);
	if (result < 0) {
		return;
	}
	total_bytes_sent += packet->size;
}

void MoQOutput::VideoData(struct encoder_packet *packet)
{
	if (video < 0) {
		VideoInit();
	}

	//auto pts = 1000000 * (packet->timebase_num * packet->pts) / packet->timebase_den;
	auto pts = util_mul_div64(
		packet->pts,
		1000000ULL * packet->timebase_num,
		packet->timebase_den
	);

	auto result = hang_track_write(video, packet->data, packet->size, pts);
	if (result < 0) {
		LOG_ERROR("Failed to write video packet: %d", result);
		return;
	}
	total_bytes_sent += packet->size;
}

void MoQOutput::VideoInit()
{
	obs_encoder_t *encoder = obs_output_get_video_encoder(output);
	if (!encoder) {
		LOG_ERROR("Failed to get video encoder");
		return;
	}

	// TODO Pass these along to the video catalog somehow.
	/*
	OBSDataAutoRelease settings = obs_encoder_get_settings(encoder);
	if (!settings) {
		LOG_ERROR("Failed to get video encoder settings");
		return;
	}

	auto video_bitrate = (int)obs_data_get_int(settings, "bitrate");
	auto video_width = obs_encoder_get_width(encoder);
	auto video_height = obs_encoder_get_height(encoder);
	*/

	const char *codec = obs_encoder_get_codec(encoder);
	video = hang_track_create(broadcast, codec);
	if (video < 0) {
		LOG_ERROR("Failed to create video track: %d", video);
		return;
	}

	uint8_t *extra_data = nullptr;
	size_t extra_size = 0;

	// obs_encoder_get_extra_data may only return data after the first frame has been encoded.
	// For H.264, this returns the SPS/PPS
	if (!obs_encoder_get_extra_data(encoder, &extra_data, &extra_size)) {
		LOG_WARNING("Failed to get extra data");
	}

	auto result = hang_track_init(video, extra_data, extra_size);
	if (result < 0) {
		LOG_ERROR("Failed to initialize video track: %d", result);
		return;
	}
}

void MoQOutput::AudioInit()
{
	obs_encoder_t *encoder = obs_output_get_audio_encoder(output, 0);
	if (!encoder) {
		LOG_ERROR("Failed to get audio encoder");
		return;
	}

	// TODO Pass these along to the audio catalog somehow.
	/*
	OBSDataAutoRelease settings = obs_encoder_get_settings(encoder);
	if (!settings) {
		LOG_ERROR("Failed to get audio encoder settings");
		return;
	}

	auto audio_bitrate = (int)obs_data_get_int(settings, "bitrate");
	*/

	const char *codec = obs_encoder_get_codec(encoder);
	audio = hang_track_create(broadcast, codec);
	if (audio < 0) {
		LOG_ERROR("Failed to create audio track: %d", audio);
		return;
	}

	uint8_t *extra_data = nullptr;
	size_t extra_size = 0;

	// obs_encoder_get_extra_data may only return data after the first frame has been encoded.
	// For AAC, this returns 2 bytes containing the profile and the sample rate.
	if (!obs_encoder_get_extra_data(encoder, &extra_data, &extra_size)) {
		LOG_WARNING("Failed to get extra data");
	}

	auto result = hang_track_init(audio, extra_data, extra_size);
	if (result < 0) {
		LOG_ERROR("Failed to initialize audio track: %d", result);
		return;
	}

	LOG_INFO("Audio track initialized successfully: %d", audio);
}

void register_moq_output()
{
	const uint32_t base_flags = OBS_OUTPUT_ENCODED | OBS_OUTPUT_SERVICE;

	// TODO: Add support for other codecs.
	const char *audio_codecs = "aac";
	// TODO: Add support for other codecs.
	const char *video_codecs = "h264";

	struct obs_output_info info = {};
	info.id = "moq_output";
	info.flags = OBS_OUTPUT_AV | base_flags;
	info.get_name = [](void *) -> const char * {
		return "MoQ Output";
	};
	info.create = [](obs_data_t *settings, obs_output_t *output) -> void * {
		return new MoQOutput(settings, output);
	};
	info.destroy = [](void *priv_data) {
		delete static_cast<MoQOutput *>(priv_data);
	};
	info.start = [](void *priv_data) -> bool {
		return static_cast<MoQOutput *>(priv_data)->Start();
	};
	info.stop = [](void *priv_data, uint64_t) {
		static_cast<MoQOutput *>(priv_data)->Stop();
	};
	info.encoded_packet = [](void *priv_data, struct encoder_packet *packet) {
		static_cast<MoQOutput *>(priv_data)->Data(packet);
	};
	info.get_total_bytes = [](void *priv_data) -> uint64_t {
		return (uint64_t)static_cast<MoQOutput *>(priv_data)->GetTotalBytes();
	};
	info.get_connect_time_ms = [](void *priv_data) -> int {
		return static_cast<MoQOutput *>(priv_data)->GetConnectTime();
	};
	info.encoded_video_codecs = video_codecs;
	info.encoded_audio_codecs = audio_codecs;
	info.protocols = "MoQ";

	obs_register_output(&info);

	info.id = "moq_output_video";
	info.flags = OBS_OUTPUT_VIDEO | base_flags;
	info.encoded_audio_codecs = nullptr;
	obs_register_output(&info);

	info.id = "moq_output_audio";
	info.flags = OBS_OUTPUT_AUDIO | base_flags;
	info.encoded_video_codecs = nullptr;
	info.encoded_audio_codecs = audio_codecs;
	obs_register_output(&info);
}
