#include <obs.hpp>

#include "moq-output.h"

extern "C" {
#include "hang.h"
}

MoQOutput::MoQOutput(obs_data_t *, obs_output_t *output)
	: output(output),
	  server_url(),
	  path(),
	  total_bytes_sent(0),
	  connect_time_ms(0)
{
	LOG_INFO("MoQOutput instance created");
}

MoQOutput::~MoQOutput()
{
	LOG_INFO("MoQOutput instance being destroyed");
	Stop();
}

bool MoQOutput::Start()
{
	LOG_INFO("Starting MoQ output...");

	ConfigureVideoTrack();
	ConfigureAudioTrack();

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

	LOG_INFO("Server URL: %s", server_url.c_str());

	path = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_STREAM_KEY);
	LOG_INFO("Stream path: %s", path.c_str());

	const obs_encoder_t *encoder = obs_output_get_video_encoder2(output, 0);

	if (!encoder) {
		LOG_ERROR("Failed to get video encoder");
		return false;
	}

	// std::cout << "joy: " << obs_encoder_get_width(encoder) << std::endl;

	obs_data_t *encoder_settings = obs_encoder_get_settings(encoder);
	const char *profile_str = obs_data_get_string(encoder_settings, "profile");

	LOG_INFO("Video encoder - Width: %d, Height: %d, Profile: %s", obs_encoder_get_width(encoder),
		 obs_encoder_get_height(encoder), profile_str ? profile_str : "none");

	LOG_DEBUG("Encoder settings: %s", obs_data_get_json_pretty(encoder_settings));

	LOG_INFO("Initializing hang library with server: %s, path: %s", server_url.c_str(), path.c_str());
	hang_start_from_c(server_url.c_str(), path.c_str(), profile_str);

	obs_data_release(encoder_settings);

	obs_output_begin_data_capture(output, 0);

	LOG_INFO("MoQ output started successfully");
	return true;
}

void MoQOutput::Stop(bool signal)
{
	LOG_INFO("Stopping MoQ output (signal: %s)", signal ? "true" : "false");

	if (signal) {
		obs_output_signal_stop(output, OBS_OUTPUT_SUCCESS);

		LOG_INFO("Stopping hang library");
		hang_stop_from_c();
		LOG_INFO("MoQ output stopped successfully. Total bytes sent: %zu", total_bytes_sent);
	}

	return;
}

void MoQOutput::Data(struct encoder_packet *packet)
{
	if (!packet) {
		LOG_ERROR("Received null packet, stopping output");
		Stop(false);
		obs_output_signal_stop(output, OBS_OUTPUT_ENCODE_ERROR);
		return;
	}

	if (packet->dts_usec < 0) {
		LOG_ERROR("Received packet with negative dts, skipping");
		return;
	}

	if (packet->type == OBS_ENCODER_AUDIO) {
		// LOG_DEBUG("Received audio packet - size: %zu, pts: %lld", packet->size, packet->pts);
		hang_write_audio_packet_from_c(packet->data, packet->size, packet->pts);
		return;
	} else if (packet->type == OBS_ENCODER_VIDEO) {
		// LOG_DEBUG("Received video packet - size: %zu, keyframe: %s, pts: %lld", packet->size,
			  // packet->keyframe ? "yes" : "no", packet->pts);
		hang_write_video_packet_from_c(packet->data, packet->size, packet->keyframe, packet->pts);
		total_bytes_sent += packet->size;
		return;
	}
}

void MoQOutput::ConfigureVideoTrack()
{
	obs_encoder_t *encoder = obs_output_get_video_encoder2(output, 0);
	if (!encoder) {
		LOG_ERROR("Failed to get video encoder");
		return;
	}

	OBSDataAutoRelease settings = obs_encoder_get_settings(encoder);
	if (!settings) {
		LOG_ERROR("Failed to get video encoder settings");
		return;
	}

	LOG_DEBUG("Video encoder settings: %s", obs_data_get_json_pretty_with_defaults(settings));

	const char *video_codec = obs_encoder_get_codec(encoder);
	const char *profile = obs_data_get_string(settings, "profile"); // Could be "" 
	auto video_bitrate = (int)obs_data_get_int(settings, "bitrate");
	auto video_width = obs_encoder_get_width(encoder);
	auto video_height = obs_encoder_get_height(encoder);

	LOG_INFO("Video codec: %s, profile: %s, bitrate: %d, width: %d, height: %d", video_codec, profile, video_bitrate,
            video_width, video_height);
	return;
}

void MoQOutput::ConfigureAudioTrack()
{
	obs_encoder_t *encoder = obs_output_get_audio_encoder(output, 0);
	if (!encoder) {
		LOG_ERROR("Failed to get audio encoder");
		return;
	}

	OBSDataAutoRelease settings = obs_encoder_get_settings(encoder);
	if (!settings) {
		LOG_ERROR("Failed to get audio encoder settings");
		return;
	}

	LOG_DEBUG("Audio encoder settings: %s", obs_data_get_json_pretty_with_defaults(settings));

	const char *audio_codec = obs_encoder_get_codec(encoder);
	auto audio_bitrate = (int)obs_data_get_int(settings, "bitrate");
	auto audio_sample_rate = obs_encoder_get_sample_rate(encoder);
	uint32_t audio_channels = 2;

	LOG_INFO("Audio codec: %s, bitrate: %d, sample rate: %d, channels: %d", audio_codec, audio_bitrate,
		 audio_sample_rate, audio_channels);
	return;
}

void register_moq_output()
{
	LOG_INFO("Registering MoQ output types");

	const uint32_t base_flags = OBS_OUTPUT_ENCODED | OBS_OUTPUT_SERVICE;

	const char *audio_codecs = "aac";
	const char *video_codecs = "h264;hevc;av1";

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
	LOG_INFO("Registered output type: moq_output (AV)");

	info.id = "moq_output_video";
	info.flags = OBS_OUTPUT_VIDEO | base_flags;
	info.encoded_audio_codecs = nullptr;
	obs_register_output(&info);
	LOG_INFO("Registered output type: moq_output_video (video-only)");

	info.id = "moq_output_audio";
	info.flags = OBS_OUTPUT_AUDIO | base_flags;
	info.encoded_video_codecs = nullptr;
	info.encoded_audio_codecs = audio_codecs;
	obs_register_output(&info);
	LOG_INFO("Registered output type: moq_output_audio (audio-only)");
}
