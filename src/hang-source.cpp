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

	// Settings - current active connection settings
	char *url;
	char *broadcast;

	// Pending settings - what user has typed but not yet applied
	char *pending_url;
	char *pending_broadcast;
	uint64_t settings_changed_time;  // Timestamp when settings last changed
	bool reconnect_pending;          // True if we need to reconnect after debounce

	// Shutdown flag - set when destroy begins, callbacks should exit early
	volatile bool shutting_down;

	// Session handles (all negative = invalid)
	volatile uint32_t generation;  // Increments on reconnect
	bool reconnect_in_progress;    // True while reconnect is happening
	int32_t origin;
	int32_t session;
	int32_t consume;
	int32_t catalog_handle;
	int32_t video_track;

	// Decoder state
	AVCodecContext *codec_ctx;
	struct SwsContext *sws_ctx;
	bool got_keyframe;
	uint32_t frames_waiting_for_keyframe;  // Count of skipped frames while waiting
	uint32_t consecutive_decode_errors;    // Count of consecutive decode failures

	// Output frame buffer
	struct obs_source_frame frame;
	uint8_t *frame_buffer;

	// Threading
	pthread_mutex_t mutex;
};

// Debounce delay in milliseconds (500ms = user stops typing for half a second)
#define DEBOUNCE_DELAY_MS 500

// Forward declarations
static void hang_source_update(void *data, obs_data_t *settings);
static void hang_source_destroy(void *data);
static void hang_source_video_tick(void *data, float seconds);
static obs_properties_t *hang_source_properties(void *data);
static void hang_source_get_defaults(obs_data_t *settings);

// MoQ callbacks
static void on_session_status(void *user_data, int32_t code);
static void on_catalog(void *user_data, int32_t catalog);
static void on_video_frame(void *user_data, int32_t frame_id);

// Helper functions
static void hang_source_reconnect(struct hang_source *ctx);
static void hang_source_disconnect_locked(struct hang_source *ctx);
static void hang_source_blank_video(struct hang_source *ctx);
static bool hang_source_init_decoder(struct hang_source *ctx, const struct moq_video_config *config);
static void hang_source_destroy_decoder_locked(struct hang_source *ctx);
static void hang_source_decode_frame(struct hang_source *ctx, int32_t frame_id);

static void *hang_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct hang_source *ctx = (struct hang_source *)bzalloc(sizeof(struct hang_source));
	ctx->source = source;

	// Initialize pending settings
	ctx->pending_url = NULL;
	ctx->pending_broadcast = NULL;
	ctx->settings_changed_time = 0;
	ctx->reconnect_pending = false;

	// Initialize shutdown flag
	ctx->shutting_down = false;

	// Initialize handles to invalid values
	ctx->generation = 0;
	ctx->reconnect_in_progress = false;
	ctx->origin = -1;
	ctx->session = -1;
	ctx->consume = -1;
	ctx->catalog_handle = -1;
	ctx->video_track = -1;

	// Initialize decoder state
	ctx->codec_ctx = NULL;
	ctx->sws_ctx = NULL;
	ctx->got_keyframe = false;
	ctx->frames_waiting_for_keyframe = 0;
	ctx->consecutive_decode_errors = 0;
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

	// Set shutdown flag first - callbacks will check this and exit early
	pthread_mutex_lock(&ctx->mutex);
	ctx->shutting_down = true;
	hang_source_disconnect_locked(ctx);
	pthread_mutex_unlock(&ctx->mutex);

	// Give MoQ callbacks time to drain - they check shutting_down and exit early
	// This prevents use-after-free when async callbacks fire after ctx is freed
	os_sleep_ms(100);

	bfree(ctx->url);
	bfree(ctx->broadcast);
	bfree(ctx->pending_url);
	bfree(ctx->pending_broadcast);
	// Note: frame_buffer is already freed by hang_source_disconnect_locked

	pthread_mutex_destroy(&ctx->mutex);

	bfree(ctx);
}

static void hang_source_update(void *data, obs_data_t *settings)
{
	struct hang_source *ctx = (struct hang_source *)data;

	const char *url = obs_data_get_string(settings, "url");
	const char *broadcast = obs_data_get_string(settings, "broadcast");

	pthread_mutex_lock(&ctx->mutex);

	// Check if pending settings have changed
	bool pending_changed = false;
	
	if (!ctx->pending_url || strcmp(ctx->pending_url, url) != 0) {
		bfree(ctx->pending_url);
		ctx->pending_url = bstrdup(url);
		pending_changed = true;
	}

	if (!ctx->pending_broadcast || strcmp(ctx->pending_broadcast, broadcast) != 0) {
		bfree(ctx->pending_broadcast);
		ctx->pending_broadcast = bstrdup(broadcast);
		pending_changed = true;
	}

	if (pending_changed) {
		// Record the time of this change and mark reconnect as pending
		// The actual reconnect will happen in video_tick after debounce delay
		ctx->settings_changed_time = os_gettime_ns();
		ctx->reconnect_pending = true;
		LOG_DEBUG("Settings changed, scheduling reconnect after debounce (url=%s, broadcast=%s)", 
		          url ? url : "(null)", broadcast ? broadcast : "(null)");
	}

	pthread_mutex_unlock(&ctx->mutex);
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

// video_tick handles debounced reconnection - waits for user to stop typing
static void hang_source_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct hang_source *ctx = (struct hang_source *)data;

	pthread_mutex_lock(&ctx->mutex);

	// Don't process during shutdown
	if (ctx->shutting_down) {
		pthread_mutex_unlock(&ctx->mutex);
		return;
	}

	if (!ctx->reconnect_pending) {
		pthread_mutex_unlock(&ctx->mutex);
		return;
	}

	// Check if enough time has passed since last settings change (debounce)
	uint64_t now = os_gettime_ns();
	uint64_t elapsed_ms = (now - ctx->settings_changed_time) / 1000000;

	if (elapsed_ms < DEBOUNCE_DELAY_MS) {
		pthread_mutex_unlock(&ctx->mutex);
		return;
	}

	// Debounce period elapsed - time to apply the pending settings
	ctx->reconnect_pending = false;

	// Check if pending settings differ from current active settings
	bool url_changed = (!ctx->url && ctx->pending_url) ||
	                   (ctx->url && !ctx->pending_url) ||
	                   (ctx->url && ctx->pending_url && strcmp(ctx->url, ctx->pending_url) != 0);
	bool broadcast_changed = (!ctx->broadcast && ctx->pending_broadcast) ||
	                         (ctx->broadcast && !ctx->pending_broadcast) ||
	                         (ctx->broadcast && ctx->pending_broadcast && strcmp(ctx->broadcast, ctx->pending_broadcast) != 0);

	if (!url_changed && !broadcast_changed) {
		// No actual change from current active connection
		pthread_mutex_unlock(&ctx->mutex);
		return;
	}

	// Apply pending settings as the new active settings
	bfree(ctx->url);
	ctx->url = ctx->pending_url ? bstrdup(ctx->pending_url) : NULL;
	bfree(ctx->broadcast);
	ctx->broadcast = ctx->pending_broadcast ? bstrdup(ctx->pending_broadcast) : NULL;

	// Check if the new settings are valid
	bool valid = ctx->url && ctx->broadcast && strlen(ctx->url) > 0 && strlen(ctx->broadcast) > 0;

	if (!valid) {
		// Invalid settings - disconnect and blank video
		LOG_INFO("Invalid URL or broadcast - disconnecting and blanking video");
		hang_source_disconnect_locked(ctx);
		pthread_mutex_unlock(&ctx->mutex);
		hang_source_blank_video(ctx);
		return;
	}

	pthread_mutex_unlock(&ctx->mutex);

	// Valid settings - reconnect
	LOG_INFO("Debounce complete, reconnecting to %s / %s", ctx->url, ctx->broadcast);
	hang_source_reconnect(ctx);
}

// Forward declaration for use in callback
static void hang_source_start_consume(struct hang_source *ctx, uint32_t expected_gen);

// MoQ callback implementations
static void on_session_status(void *user_data, int32_t code)
{
	struct hang_source *ctx = (struct hang_source *)user_data;

	// Check if we're shutting down - exit early to avoid use-after-free
	pthread_mutex_lock(&ctx->mutex);
	if (ctx->shutting_down) {
		LOG_DEBUG("Ignoring session status callback - shutting down");
		pthread_mutex_unlock(&ctx->mutex);
		return;
	}
	if (ctx->session < 0) {
		LOG_DEBUG("Ignoring session status callback - already disconnected");
		pthread_mutex_unlock(&ctx->mutex);
		return;
	}
	uint32_t current_gen = ctx->generation;
	
	if (code == 0) {
		pthread_mutex_unlock(&ctx->mutex);
		LOG_INFO("MoQ session connected successfully (generation %u)", current_gen);
		// Now that we're connected, start consuming the broadcast
		hang_source_start_consume(ctx, current_gen);
	} else {
		// Connection failed - clean up the session and origin immediately
		LOG_ERROR("MoQ session failed with code: %d (generation %u)", code, current_gen);
		
		// Clean up failed session/origin to prevent further callbacks
		if (ctx->session >= 0) {
			moq_session_close(ctx->session);
			ctx->session = -1;
		}
		if (ctx->origin >= 0) {
			moq_origin_close(ctx->origin);
			ctx->origin = -1;
		}
		pthread_mutex_unlock(&ctx->mutex);
		
		// Blank the video to show error state
		hang_source_blank_video(ctx);
	}
}

static void on_catalog(void *user_data, int32_t catalog)
{
	struct hang_source *ctx = (struct hang_source *)user_data;

	LOG_INFO("Catalog callback received: %d", catalog);

	pthread_mutex_lock(&ctx->mutex);

	// Check if we're shutting down - exit early to avoid use-after-free
	if (ctx->shutting_down) {
		LOG_DEBUG("Ignoring catalog callback - shutting down");
		pthread_mutex_unlock(&ctx->mutex);
		if (catalog >= 0)
			moq_consume_catalog_close(catalog);
		return;
	}

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
		// Catalog failed (likely invalid broadcast) - blank video
		hang_source_blank_video(ctx);
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

	// Check if this callback is still valid using generation (not video_track)
	// Note: We can't check video_track here because frames may arrive before
	// the track handle is stored in on_catalog (race condition)
	pthread_mutex_lock(&ctx->mutex);
	// Check if we're shutting down - exit early to avoid use-after-free
	if (ctx->shutting_down) {
		pthread_mutex_unlock(&ctx->mutex);
		moq_consume_frame_close(frame_id);
		return;
	}
	if (ctx->consume < 0) {
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
	
	// Check if reconnect is already in progress
	if (ctx->reconnect_in_progress) {
		LOG_DEBUG("Reconnect already in progress, skipping");
		pthread_mutex_unlock(&ctx->mutex);
		return;
	}
	
	ctx->reconnect_in_progress = true;
	uint32_t new_gen = ctx->generation + 1;
	LOG_INFO("Reconnecting (generation %u -> %u)", ctx->generation, new_gen);
	ctx->generation = new_gen;
	hang_source_disconnect_locked(ctx);

	// Copy URL while holding mutex for thread safety
	char *url_copy = bstrdup(ctx->url);
	pthread_mutex_unlock(&ctx->mutex);

	// Blank video while reconnecting to avoid showing stale frames
	hang_source_blank_video(ctx);

	// Small delay to allow MoQ library to fully clean up previous connection
	os_sleep_ms(50);

	// Create origin for consuming (outside mutex since it may block)
	int32_t new_origin = moq_origin_create();
	if (new_origin < 0) {
		LOG_ERROR("Failed to create origin: %d", new_origin);
		bfree(url_copy);
		pthread_mutex_lock(&ctx->mutex);
		ctx->reconnect_in_progress = false;
		pthread_mutex_unlock(&ctx->mutex);
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
		pthread_mutex_lock(&ctx->mutex);
		ctx->reconnect_in_progress = false;
		pthread_mutex_unlock(&ctx->mutex);
		return;
	}

	// Now update ctx with the new handles, checking if generation changed
	pthread_mutex_lock(&ctx->mutex);
	if (ctx->generation != new_gen) {
		// Another reconnect happened while we were creating origin/session
		// Clean up our newly created resources
		ctx->reconnect_in_progress = false;
		pthread_mutex_unlock(&ctx->mutex);
		LOG_INFO("Generation changed during reconnect setup, cleaning up stale resources");
		moq_session_close(new_session);
		moq_origin_close(new_origin);
		return;
	}
	ctx->origin = new_origin;
	ctx->session = new_session;
	ctx->reconnect_in_progress = false;
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
		LOG_ERROR("Failed to consume broadcast '%s': %d", broadcast_copy, consume);
		bfree(broadcast_copy);
		// Failed to consume - clean up session/origin
		pthread_mutex_lock(&ctx->mutex);
		if (ctx->generation == expected_gen) {
			if (ctx->session >= 0) {
				moq_session_close(ctx->session);
				ctx->session = -1;
			}
			if (ctx->origin >= 0) {
				moq_origin_close(ctx->origin);
				ctx->origin = -1;
			}
		}
		pthread_mutex_unlock(&ctx->mutex);
		hang_source_blank_video(ctx);
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
		LOG_ERROR("Failed to subscribe to catalog for '%s': %d", broadcast_copy, catalog_handle);
		bfree(broadcast_copy);
		// Failed to get catalog - clean up
		pthread_mutex_lock(&ctx->mutex);
		if (ctx->generation == expected_gen) {
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
		}
		pthread_mutex_unlock(&ctx->mutex);
		hang_source_blank_video(ctx);
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
	ctx->frames_waiting_for_keyframe = 0;
	ctx->consecutive_decode_errors = 0;
}

// Blanks the video preview by outputting a NULL frame
static void hang_source_blank_video(struct hang_source *ctx)
{
	// Passing NULL to obs_source_output_video clears the current frame
	obs_source_output_video(ctx->source, NULL);
	LOG_DEBUG("Video preview blanked");
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
	ctx->frames_waiting_for_keyframe = 0;
	ctx->consecutive_decode_errors = 0;

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

	// Check if we're shutting down - exit early to avoid use-after-free
	if (ctx->shutting_down) {
		pthread_mutex_unlock(&ctx->mutex);
		moq_consume_frame_close(frame_id);
		return;
	}

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
		ctx->frames_waiting_for_keyframe++;
		if (ctx->frames_waiting_for_keyframe == 1 || 
		    (ctx->frames_waiting_for_keyframe % 30) == 0) {
			LOG_INFO("Waiting for keyframe... (skipped %u frames so far)", 
			         ctx->frames_waiting_for_keyframe);
		}
		pthread_mutex_unlock(&ctx->mutex);
		moq_consume_frame_close(frame_id);
		return;
	}

	// Mark that we've received a keyframe from the stream
	if (frame_data.keyframe) {
		if (!ctx->got_keyframe) {
			LOG_INFO("Got keyframe after waiting for %u frames, payload_size=%zu", 
			         ctx->frames_waiting_for_keyframe, frame_data.payload_size);
			// Flush decoder to ensure clean state when starting from keyframe
			avcodec_flush_buffers(ctx->codec_ctx);
		}
		ctx->got_keyframe = true;
		ctx->frames_waiting_for_keyframe = 0;
		ctx->consecutive_decode_errors = 0;
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
			ctx->consecutive_decode_errors++;
			char errbuf[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(ret, errbuf, sizeof(errbuf));
			
			// If too many consecutive errors, flush decoder and wait for next keyframe
			if (ctx->consecutive_decode_errors >= 5) {
				LOG_WARNING("Too many send errors (%u), flushing decoder and waiting for keyframe", 
				            ctx->consecutive_decode_errors);
				avcodec_flush_buffers(ctx->codec_ctx);
				ctx->got_keyframe = false;
				ctx->consecutive_decode_errors = 0;
			} else if (ctx->consecutive_decode_errors == 1) {
				LOG_ERROR("Error sending packet to decoder: %s", errbuf);
			}
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
			ctx->consecutive_decode_errors++;
			char errbuf[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(ret, errbuf, sizeof(errbuf));
			
			// If too many consecutive errors, flush decoder and wait for next keyframe
			if (ctx->consecutive_decode_errors >= 5) {
				LOG_WARNING("Too many decode errors (%u), flushing decoder and waiting for keyframe", 
				            ctx->consecutive_decode_errors);
				avcodec_flush_buffers(ctx->codec_ctx);
				ctx->got_keyframe = false;
				ctx->consecutive_decode_errors = 0;
			} else if (ctx->consecutive_decode_errors == 1) {
				// Only log first error in a sequence
				LOG_ERROR("Error receiving frame from decoder: %s", errbuf);
			}
		}
		av_frame_free(&frame);
		pthread_mutex_unlock(&ctx->mutex);
		moq_consume_frame_close(frame_id);
		return;
	}
	
	// Successfully decoded a frame - reset error counter
	ctx->consecutive_decode_errors = 0;

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
	// video_tick is needed for debounced reconnection (blur simulation)
	info.video_tick = hang_source_video_tick;

	obs_register_source(&info);
}
