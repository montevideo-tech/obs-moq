#include <obs-module.h>
#include <util/threading.h>
#include <util/platform.h>
#include <util/darray.h>
#include <util/dstr.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include "moq.h"
}

#include "hang-source.h"
#include "logger.h"

#define FRAME_WIDTH 1920
#define FRAME_HEIGHT 1080

struct hang_source {
	obs_source_t *source;

	// Settings
	char *url;
	char *broadcast;

	// Session handles (all negative = invalid)
	volatile uint32_t generation;  // Increments on reconnect
	int32_t origin;
	int32_t session;
	int32_t consume;
	int32_t catalog_handle;
	int32_t video_track;

	// Decoder state
	AVCodecContext *codec_ctx;
	struct SwsContext *sws_ctx;
	bool got_keyframe;

	// Output frame buffer
	struct obs_source_frame frame;
	uint8_t *frame_buffer;

	// Threading
	pthread_mutex_t mutex;
};

// Forward declarations
static void hang_source_update(void *data, obs_data_t *settings);
static void hang_source_destroy(void *data);
static obs_properties_t *hang_source_properties(void *data);
static void hang_source_get_defaults(obs_data_t *settings);

// MoQ callbacks
static void on_session_status(void *user_data, int32_t code);
static void on_catalog(void *user_data, int32_t catalog);
static void on_video_frame(void *user_data, int32_t frame_id);

// Helper functions
static void hang_source_reconnect(struct hang_source *ctx);
static void hang_source_disconnect_locked(struct hang_source *ctx);
static bool hang_source_init_decoder(struct hang_source *ctx, const struct moq_video_config *config);
static void hang_source_destroy_decoder_locked(struct hang_source *ctx);
static void hang_source_decode_frame(struct hang_source *ctx, int32_t frame_id);

static void *hang_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct hang_source *ctx = (struct hang_source *)bzalloc(sizeof(struct hang_source));
	ctx->source = source;

	// Initialize handles to invalid values
	ctx->generation = 0;
	ctx->origin = -1;
	ctx->session = -1;
	ctx->consume = -1;
	ctx->catalog_handle = -1;
	ctx->video_track = -1;

	// Initialize decoder state
	ctx->codec_ctx = NULL;
	ctx->sws_ctx = NULL;
	ctx->got_keyframe = false;
	ctx->frame_buffer = NULL;

	// Initialize threading
	pthread_mutex_init(&ctx->mutex, NULL);

	// Initialize OBS frame structure
	ctx->frame.width = FRAME_WIDTH;
	ctx->frame.height = FRAME_HEIGHT;
	ctx->frame.format = VIDEO_FORMAT_RGBA;
	ctx->frame.linesize[0] = FRAME_WIDTH * 4;

	hang_source_update(ctx, settings);

	return ctx;
}

static void hang_source_destroy(void *data)
{
	struct hang_source *ctx = (struct hang_source *)data;

	pthread_mutex_lock(&ctx->mutex);
	hang_source_disconnect_locked(ctx);
	pthread_mutex_unlock(&ctx->mutex);

	bfree(ctx->url);
	bfree(ctx->broadcast);
	// Note: frame_buffer is already freed by hang_source_disconnect_locked

	pthread_mutex_destroy(&ctx->mutex);

	bfree(ctx);
}

static void hang_source_update(void *data, obs_data_t *settings)
{
	struct hang_source *ctx = (struct hang_source *)data;

	const char *url = obs_data_get_string(settings, "url");
	const char *broadcast = obs_data_get_string(settings, "broadcast");

	bool changed = false;
	bool should_reconnect = false;

	pthread_mutex_lock(&ctx->mutex);

	if (!ctx->url || strcmp(ctx->url, url) != 0) {
		bfree(ctx->url);
		ctx->url = bstrdup(url);
		changed = true;
	}

	if (!ctx->broadcast || strcmp(ctx->broadcast, broadcast) != 0) {
		bfree(ctx->broadcast);
		ctx->broadcast = bstrdup(broadcast);
		changed = true;
	}

	if (changed && ctx->url && ctx->broadcast && strlen(ctx->url) > 0 && strlen(ctx->broadcast) > 0) {
		should_reconnect = true;
	}

	pthread_mutex_unlock(&ctx->mutex);

	if (should_reconnect) {
		hang_source_reconnect(ctx);
	}
}

static void hang_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "url", "https://attention.us-central-2.ooda.video:4443");
	obs_data_set_default_string(settings, "broadcast", "flyover-ranch/cam_192_168_42_190");
}

static obs_properties_t *hang_source_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "url", "URL", OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "broadcast", "Broadcast", OBS_TEXT_DEFAULT);

	return props;
}

// Note: We use OBS_SOURCE_ASYNC_VIDEO, so OBS handles rendering automatically
// via obs_source_output_video(). No video_tick or video_render callbacks needed.

// Forward declaration for use in callback
static void hang_source_start_consume(struct hang_source *ctx, uint32_t expected_gen);

// MoQ callback implementations
static void on_session_status(void *user_data, int32_t code)
{
	struct hang_source *ctx = (struct hang_source *)user_data;

	// Check if we've been disconnected and capture generation
	pthread_mutex_lock(&ctx->mutex);
	if (ctx->session < 0) {
		pthread_mutex_unlock(&ctx->mutex);
		return;
	}
	uint32_t current_gen = ctx->generation;
	pthread_mutex_unlock(&ctx->mutex);

	if (code == 0) {
		LOG_INFO("MoQ session connected successfully (generation %u)", current_gen);
		// Now that we're connected, start consuming the broadcast
		hang_source_start_consume(ctx, current_gen);
	} else {
		LOG_ERROR("MoQ session failed with code: %d", code);
	}
}

static void on_catalog(void *user_data, int32_t catalog)
{
	struct hang_source *ctx = (struct hang_source *)user_data;

	LOG_INFO("Catalog callback received: %d", catalog);

	pthread_mutex_lock(&ctx->mutex);

	// Check if this callback is still valid (not from a stale connection)
	uint32_t current_gen = ctx->generation;
	if (ctx->consume < 0) {
		// We've been disconnected, ignore this callback
		pthread_mutex_unlock(&ctx->mutex);
		if (catalog >= 0)
			moq_consume_catalog_close(catalog);
		return;
	}

	pthread_mutex_unlock(&ctx->mutex);

	if (catalog < 0) {
		LOG_ERROR("Failed to get catalog: %d", catalog);
		return;
	}

	// Get video configuration
	struct moq_video_config video_config;
	if (moq_consume_video_config(catalog, 0, &video_config) < 0) {
		LOG_ERROR("Failed to get video config");
		moq_consume_catalog_close(catalog);
		return;
	}

	// Initialize decoder with the video config (takes mutex internally)
	if (!hang_source_init_decoder(ctx, &video_config)) {
		LOG_ERROR("Failed to initialize decoder");
		moq_consume_catalog_close(catalog);
		return;
	}

	// Subscribe to video track with minimal buffering
	// Note: moq_consume_video_ordered takes the catalog handle, not the consume handle
	int32_t track = moq_consume_video_ordered(catalog, 0, 0, on_video_frame, ctx);
	if (track < 0) {
		LOG_ERROR("Failed to subscribe to video track: %d", track);
		moq_consume_catalog_close(catalog);
		return;
	}

	pthread_mutex_lock(&ctx->mutex);
	if (ctx->generation == current_gen) {
		ctx->video_track = track;
		ctx->catalog_handle = catalog;
	} else {
		// Generation changed while we were setting up, clean up the track
		pthread_mutex_unlock(&ctx->mutex);
		moq_consume_video_close(track);
		moq_consume_catalog_close(catalog);
		return;
	}
	pthread_mutex_unlock(&ctx->mutex);

	LOG_INFO("Subscribed to video track successfully");
}

static void on_video_frame(void *user_data, int32_t frame_id)
{
	struct hang_source *ctx = (struct hang_source *)user_data;

	if (frame_id < 0) {
		LOG_ERROR("Video frame callback with error: %d", frame_id);
		return;
	}

	// Check if this callback is still valid (not from a stale connection)
	pthread_mutex_lock(&ctx->mutex);
	if (ctx->video_track < 0) {
		// We've been disconnected, ignore this callback
		pthread_mutex_unlock(&ctx->mutex);
		moq_consume_frame_close(frame_id);
		return;
	}
	pthread_mutex_unlock(&ctx->mutex);

	hang_source_decode_frame(ctx, frame_id);
}

// Helper function implementations
static void hang_source_reconnect(struct hang_source *ctx)
{
	// Increment generation to invalidate old callbacks
	pthread_mutex_lock(&ctx->mutex);
	uint32_t new_gen = ctx->generation + 1;
	LOG_INFO("Reconnecting (generation %u -> %u)", ctx->generation, new_gen);
	ctx->generation = new_gen;
	hang_source_disconnect_locked(ctx);

	// Copy URL while holding mutex for thread safety
	char *url_copy = bstrdup(ctx->url);
	pthread_mutex_unlock(&ctx->mutex);

	// Create origin for consuming (outside mutex since it may block)
	int32_t new_origin = moq_origin_create();
	if (new_origin < 0) {
		LOG_ERROR("Failed to create origin: %d", new_origin);
		bfree(url_copy);
		return;
	}

	// Connect to MoQ server (consume will happen in on_session_status callback)
	int32_t new_session = moq_session_connect(
		url_copy, strlen(url_copy),
		0, // origin_publish
		new_origin, // origin_consume
		on_session_status, ctx
	);
	bfree(url_copy);

	if (new_session < 0) {
		LOG_ERROR("Failed to connect to MoQ server: %d", new_session);
		moq_origin_close(new_origin);
		return;
	}

	// Now update ctx with the new handles, checking if generation changed
	pthread_mutex_lock(&ctx->mutex);
	if (ctx->generation != new_gen) {
		// Another reconnect happened while we were creating origin/session
		// Clean up our newly created resources
		pthread_mutex_unlock(&ctx->mutex);
		LOG_INFO("Generation changed during reconnect setup, cleaning up stale resources");
		moq_session_close(new_session);
		moq_origin_close(new_origin);
		return;
	}
	ctx->origin = new_origin;
	ctx->session = new_session;
	LOG_INFO("Connecting to MoQ server (generation %u)", new_gen);
	pthread_mutex_unlock(&ctx->mutex);
}

// Called after session is connected successfully
static void hang_source_start_consume(struct hang_source *ctx, uint32_t expected_gen)
{
	// Check if origin is still valid and generation matches
	pthread_mutex_lock(&ctx->mutex);
	if (ctx->origin < 0 || ctx->generation != expected_gen) {
		pthread_mutex_unlock(&ctx->mutex);
		LOG_INFO("Skipping stale consume (generation mismatch or invalid origin)");
		return;
	}
	// Capture values while holding mutex
	int32_t origin = ctx->origin;
	char *broadcast_copy = bstrdup(ctx->broadcast);
	pthread_mutex_unlock(&ctx->mutex);

	// Consume broadcast by path
	int32_t consume = moq_origin_consume(origin, broadcast_copy, strlen(broadcast_copy));
	if (consume < 0) {
		LOG_ERROR("Failed to consume broadcast: %d", consume);
		bfree(broadcast_copy);
		return;
	}

	pthread_mutex_lock(&ctx->mutex);
	// Verify generation hasn't changed while we were waiting
	if (ctx->generation != expected_gen) {
		pthread_mutex_unlock(&ctx->mutex);
		LOG_INFO("Generation changed during consume setup, cleaning up");
		moq_consume_close(consume);
		bfree(broadcast_copy);
		return;
	}
	ctx->consume = consume;
	pthread_mutex_unlock(&ctx->mutex);

	// Subscribe to catalog updates
	int32_t catalog_handle = moq_consume_catalog(consume, on_catalog, ctx);
	if (catalog_handle < 0) {
		LOG_ERROR("Failed to subscribe to catalog: %d", catalog_handle);
		bfree(broadcast_copy);
		return;
	}

	LOG_INFO("Consuming broadcast: %s", broadcast_copy);
	bfree(broadcast_copy);
}

// NOTE: Caller must hold ctx->mutex when calling this function
static void hang_source_disconnect_locked(struct hang_source *ctx)
{
	if (ctx->video_track >= 0) {
		moq_consume_video_close(ctx->video_track);
		ctx->video_track = -1;
	}

	if (ctx->catalog_handle >= 0) {
		moq_consume_catalog_close(ctx->catalog_handle);
		ctx->catalog_handle = -1;
	}

	if (ctx->consume >= 0) {
		moq_consume_close(ctx->consume);
		ctx->consume = -1;
	}

	if (ctx->session >= 0) {
		moq_session_close(ctx->session);
		ctx->session = -1;
	}

	if (ctx->origin >= 0) {
		moq_origin_close(ctx->origin);
		ctx->origin = -1;
	}

	hang_source_destroy_decoder_locked(ctx);
	ctx->got_keyframe = false;
}

static bool hang_source_init_decoder(struct hang_source *ctx, const struct moq_video_config *config)
{
	// Find H.264 decoder (can be done outside mutex)
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!codec) {
		LOG_ERROR("H.264 codec not found");
		return false;
	}

	// Create codec context (can be done outside mutex)
	AVCodecContext *new_codec_ctx = avcodec_alloc_context3(codec);
	if (!new_codec_ctx) {
		LOG_ERROR("Failed to allocate codec context");
		return false;
	}

	uint32_t width = FRAME_WIDTH;
	uint32_t height = FRAME_HEIGHT;

	// Set codec parameters from config
	if (config->coded_width && *config->coded_width > 0) {
		new_codec_ctx->width = *config->coded_width;
		width = *config->coded_width;
	}
	if (config->coded_height && *config->coded_height > 0) {
		new_codec_ctx->height = *config->coded_height;
		height = *config->coded_height;
	}

	// Use codec description as extradata (contains SPS/PPS)
	if (config->description && config->description_len > 0) {
		new_codec_ctx->extradata = (uint8_t *)av_malloc(config->description_len + AV_INPUT_BUFFER_PADDING_SIZE);
		if (new_codec_ctx->extradata) {
			memcpy(new_codec_ctx->extradata, config->description, config->description_len);
			new_codec_ctx->extradata_size = config->description_len;
		}
	}

	// Open codec
	if (avcodec_open2(new_codec_ctx, codec, NULL) < 0) {
		LOG_ERROR("Failed to open codec");
		avcodec_free_context(&new_codec_ctx);
		return false;
	}

	// Allocate frame buffer (RGBA for OBS)
	size_t buffer_size = width * height * 4;
	uint8_t *new_frame_buffer = (uint8_t *)bmalloc(buffer_size);
	if (!new_frame_buffer) {
		LOG_ERROR("Failed to allocate frame buffer");
		avcodec_free_context(&new_codec_ctx);
		return false;
	}

	// Create scaling context for YUV420P -> RGBA conversion
	struct SwsContext *new_sws_ctx = sws_getContext(
		width, height, AV_PIX_FMT_YUV420P,
		width, height, AV_PIX_FMT_RGBA,
		SWS_BILINEAR, NULL, NULL, NULL
	);

	if (!new_sws_ctx) {
		LOG_ERROR("Failed to create scaling context");
		bfree(new_frame_buffer);
		avcodec_free_context(&new_codec_ctx);
		return false;
	}

	// Now take the mutex and swap in the new decoder state
	pthread_mutex_lock(&ctx->mutex);

	// Destroy old decoder state
	if (ctx->sws_ctx) {
		sws_freeContext(ctx->sws_ctx);
	}
	if (ctx->codec_ctx) {
		avcodec_free_context(&ctx->codec_ctx);
	}
	if (ctx->frame_buffer) {
		bfree(ctx->frame_buffer);
	}

	// Install new decoder state
	ctx->codec_ctx = new_codec_ctx;
	ctx->sws_ctx = new_sws_ctx;
	ctx->frame_buffer = new_frame_buffer;
	ctx->frame.width = width;
	ctx->frame.height = height;
	ctx->frame.linesize[0] = width * 4;
	ctx->frame.data[0] = new_frame_buffer;
	ctx->frame.format = VIDEO_FORMAT_RGBA;
	ctx->frame.timestamp = 0;
	ctx->got_keyframe = false;

	pthread_mutex_unlock(&ctx->mutex);

	LOG_INFO("Decoder initialized: %dx%d", width, height);
	return true;
}

// NOTE: Caller must hold ctx->mutex when calling this function
static void hang_source_destroy_decoder_locked(struct hang_source *ctx)
{
	if (ctx->sws_ctx) {
		sws_freeContext(ctx->sws_ctx);
		ctx->sws_ctx = NULL;
	}

	if (ctx->codec_ctx) {
		avcodec_free_context(&ctx->codec_ctx);
		ctx->codec_ctx = NULL;
	}

	if (ctx->frame_buffer) {
		bfree(ctx->frame_buffer);
		ctx->frame_buffer = NULL;
		ctx->frame.data[0] = NULL;
	}
}

static void hang_source_decode_frame(struct hang_source *ctx, int32_t frame_id)
{
	pthread_mutex_lock(&ctx->mutex);

	// Check if decoder is still valid (may have been destroyed during reconnect)
	if (!ctx->codec_ctx || !ctx->sws_ctx || !ctx->frame_buffer) {
		pthread_mutex_unlock(&ctx->mutex);
		moq_consume_frame_close(frame_id);
		return;
	}

	// Get frame data
	struct moq_frame frame_data;
	if (moq_consume_frame_chunk(frame_id, 0, &frame_data) < 0) {
		LOG_ERROR("Failed to get frame data");
		pthread_mutex_unlock(&ctx->mutex);
		moq_consume_frame_close(frame_id);
		return;
	}

	// Skip non-keyframes until we get the first one
	if (!ctx->got_keyframe && !frame_data.keyframe) {
		LOG_DEBUG("Skipping non-keyframe before first keyframe");
		pthread_mutex_unlock(&ctx->mutex);
		moq_consume_frame_close(frame_id);
		return;
	}

	// Mark that we've received a keyframe from the stream
	if (frame_data.keyframe) {
		ctx->got_keyframe = true;
		LOG_INFO("Received keyframe, payload_size=%zu", frame_data.payload_size);
	}

	// Create AVPacket from frame data
	AVPacket *packet = av_packet_alloc();
	if (!packet) {
		pthread_mutex_unlock(&ctx->mutex);
		moq_consume_frame_close(frame_id);
		return;
	}

	packet->data = (uint8_t *)frame_data.payload;
	packet->size = frame_data.payload_size;
	packet->pts = frame_data.timestamp_us / 1000; // Convert to milliseconds
	packet->dts = packet->pts;

	// Send packet to decoder
	int ret = avcodec_send_packet(ctx->codec_ctx, packet);
	av_packet_free(&packet);

	if (ret < 0) {
		if (ret != AVERROR(EAGAIN)) {
			char errbuf[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(ret, errbuf, sizeof(errbuf));
			LOG_ERROR("Error sending packet to decoder: %s", errbuf);
		}
		pthread_mutex_unlock(&ctx->mutex);
		moq_consume_frame_close(frame_id);
		return;
	}

	// Receive decoded frames
	AVFrame *frame = av_frame_alloc();
	if (!frame) {
		pthread_mutex_unlock(&ctx->mutex);
		moq_consume_frame_close(frame_id);
		return;
	}

	ret = avcodec_receive_frame(ctx->codec_ctx, frame);
	if (ret < 0) {
		if (ret != AVERROR(EAGAIN)) {
			char errbuf[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(ret, errbuf, sizeof(errbuf));
			LOG_ERROR("Error receiving frame from decoder: %s", errbuf);
		}
		av_frame_free(&frame);
		pthread_mutex_unlock(&ctx->mutex);
		moq_consume_frame_close(frame_id);
		return;
	}

	// Convert YUV420P to RGBA
	uint8_t *dst_data[4] = {ctx->frame_buffer, NULL, NULL, NULL};
	int dst_linesize[4] = {static_cast<int>(ctx->frame.width * 4), 0, 0, 0};

	sws_scale(ctx->sws_ctx, (const uint8_t *const *)frame->data, frame->linesize,
	          0, ctx->frame.height, dst_data, dst_linesize);

	// Update OBS frame timestamp and output
	ctx->frame.timestamp = frame_data.timestamp_us;
	obs_source_output_video(ctx->source, &ctx->frame);

	av_frame_free(&frame);
	pthread_mutex_unlock(&ctx->mutex);
	moq_consume_frame_close(frame_id);
}

// Registration function
void register_hang_source()
{
	struct obs_source_info info = {};
	info.id = "hang_source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.get_name = [](void *) -> const char * {
		return "Hang Source (MoQ)";
	};
	info.create = hang_source_create;
	info.destroy = hang_source_destroy;
	info.update = hang_source_update;
	info.get_defaults = hang_source_get_defaults;
	info.get_properties = hang_source_properties;
	// Note: No video_tick or video_render needed for async video sources
	// OBS handles rendering via obs_source_output_video()

	obs_register_source(&info);
}
