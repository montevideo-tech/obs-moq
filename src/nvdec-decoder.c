/*
FFmpeg Software Video Decoder for OBS Hang Source
Copyright (C) 2024 OBS Plugin Template

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include "logger-c.h"
#include <util/threading.h>
#include <util/platform.h>
#include <graphics/graphics.h>
#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

#include "hang-source.h"

// Function declarations
static bool decode_frame(struct nvdec_decoder *decoder, const uint8_t *data, size_t size, uint64_t pts, struct hang_source *context);
static void output_decoded_frame(struct hang_source *context, uint8_t *data, uint32_t width, uint32_t height, uint64_t pts_us);
static bool convert_mp4_nal_units_to_annex_b(const uint8_t *data, size_t size, uint8_t **out_data, size_t *out_size);

// Parse codec string to FFmpeg codec ID
// Supports: avc1/h264, hev1/hvc1/hevc, av01/av1
static enum AVCodecID parse_codec_id(const char *codec, size_t codec_len)
{
	if (!codec || codec_len == 0) {
		return AV_CODEC_ID_H264; // Default to H.264
	}

	// H.264/AVC: "avc1.*" or "h264"
	if ((codec_len >= 4 && strncmp(codec, "avc1", 4) == 0) ||
	    (codec_len >= 4 && strncasecmp(codec, "h264", 4) == 0)) {
		return AV_CODEC_ID_H264;
	}

	// HEVC/H.265: "hev1.*", "hvc1.*", or "hevc"
	if ((codec_len >= 4 && strncmp(codec, "hev1", 4) == 0) ||
	    (codec_len >= 4 && strncmp(codec, "hvc1", 4) == 0) ||
	    (codec_len >= 4 && strncasecmp(codec, "hevc", 4) == 0) ||
	    (codec_len >= 4 && strncasecmp(codec, "h265", 4) == 0)) {
		return AV_CODEC_ID_HEVC;
	}

	// AV1: "av01.*" or "av1"
	if ((codec_len >= 4 && strncmp(codec, "av01", 4) == 0) ||
	    (codec_len >= 3 && strncasecmp(codec, "av1", 3) == 0)) {
		return AV_CODEC_ID_AV1;
	}

	CLOG_WARNING("Unknown codec: %.*s, defaulting to H.264", (int)codec_len, codec);
	return AV_CODEC_ID_H264;
}

struct nvdec_decoder {
	AVCodecContext *codec_ctx;
	struct SwsContext *sws_ctx;
};

bool nvdec_decoder_init(struct hang_source *context, const char *codec_str, size_t codec_len,
                        const uint8_t *description, size_t description_len)
{
	struct nvdec_decoder *decoder = bzalloc(sizeof(struct nvdec_decoder));
	context->nvdec_context = decoder;

	// Parse the codec string to get FFmpeg codec ID
	enum AVCodecID codec_id = parse_codec_id(codec_str, codec_len);
	CLOG_INFO("Initializing video decoder for codec: %.*s (ffmpeg id: %d)",
	          (int)codec_len, codec_str ? codec_str : "unknown", codec_id);

	// Initialize FFmpeg software decoder
	const AVCodec *codec = avcodec_find_decoder(codec_id);
	if (!codec) {
		CLOG_ERROR("Codec not found for id: %d", codec_id);
		bfree(decoder);
		context->nvdec_context = NULL;
		return false;
	}

	decoder->codec_ctx = avcodec_alloc_context3(codec);
	if (!decoder->codec_ctx) {
		CLOG_ERROR("Failed to allocate codec context");
		bfree(decoder);
		context->nvdec_context = NULL;
		return false;
	}

	// Set extradata if provided (AVCC format)
	if (description && description_len > 0) {
		decoder->codec_ctx->extradata = av_mallocz(description_len + AV_INPUT_BUFFER_PADDING_SIZE);
		if (decoder->codec_ctx->extradata) {
			memcpy(decoder->codec_ctx->extradata, description, description_len);
			decoder->codec_ctx->extradata_size = description_len;
			CLOG_INFO("Set codec extradata (%zu bytes)", description_len);
		}
	}

	if (avcodec_open2(decoder->codec_ctx, codec, NULL) < 0) {
		CLOG_ERROR("Failed to open codec");
		avcodec_free_context(&decoder->codec_ctx);
		bfree(decoder);
		context->nvdec_context = NULL;
		return false;
	}

	CLOG_INFO("FFmpeg software decoder initialized with codec: %s", codec->name);
	return true;
}

void nvdec_decoder_destroy(struct hang_source *context)
{
	struct nvdec_decoder *decoder = context->nvdec_context;
	if (!decoder) {
		return;
	}

	if (decoder->codec_ctx) {
		avcodec_free_context(&decoder->codec_ctx);
		decoder->codec_ctx = NULL;
	}

	if (decoder->sws_ctx) {
		sws_freeContext(decoder->sws_ctx);
		decoder->sws_ctx = NULL;
	}

	bfree(decoder);
	context->nvdec_context = NULL;
}

bool nvdec_decoder_decode(struct hang_source *context, const uint8_t *data, size_t size, uint64_t pts, bool keyframe)
{
	UNUSED_PARAMETER(keyframe);
	struct nvdec_decoder *decoder = context->nvdec_context;
	if (!decoder) {
		return false;
	}

	return decode_frame(decoder, data, size, pts, context);
}

static bool convert_mp4_nal_units_to_annex_b(const uint8_t *data, size_t size, uint8_t **out_data, size_t *out_size)
{
	// Estimate output size (add 4 bytes for each start code, remove 4 bytes for each length)
	size_t estimated_size = size + 1024; // Add some padding
	uint8_t *buffer = bzalloc(estimated_size);
	if (!buffer) {
		return false;
	}

	size_t out_pos = 0;
	size_t pos = 0;

	while (pos + 4 <= size) {
		// Read 4-byte length
		uint32_t nal_length = (data[pos] << 24) | (data[pos + 1] << 16) | (data[pos + 2] << 8) | data[pos + 3];
		pos += 4;

		if (pos + nal_length > size) {
			CLOG_ERROR( "Invalid NAL length: %u (pos=%zu, size=%zu)", nal_length, pos, size);
			bfree(buffer);
			return false;
		}

		// Check if we need more space
		if (out_pos + 4 + nal_length > estimated_size) {
			estimated_size = out_pos + 4 + nal_length + 1024;
			uint8_t *new_buffer = brealloc(buffer, estimated_size);
			if (!new_buffer) {
				bfree(buffer);
				return false;
			}
			buffer = new_buffer;
		}

		// Write start code
		buffer[out_pos++] = 0x00;
		buffer[out_pos++] = 0x00;
		buffer[out_pos++] = 0x00;
		buffer[out_pos++] = 0x01;

		// Copy NAL data
		memcpy(buffer + out_pos, data + pos, nal_length);
		out_pos += nal_length;
		pos += nal_length;
	}

	*out_data = buffer;
	*out_size = out_pos;
	return true;
}

static bool decode_frame(struct nvdec_decoder *decoder, const uint8_t *data, size_t size, uint64_t pts, struct hang_source *context)
{
	// For MP4 H.264 (avc1), convert length-prefixed NAL units to start-code format
	uint8_t *converted_data = NULL;
	size_t converted_size = 0;

	if (!convert_mp4_nal_units_to_annex_b(data, size, &converted_data, &converted_size)) {
		CLOG_ERROR( "Failed to convert NAL units: size=%zu", size);
		return false;
	}

	AVPacket *packet = av_packet_alloc();
	if (!packet) {
		CLOG_ERROR( "Failed to allocate AVPacket");
		bfree(converted_data);
		return false;
	}

	packet->data = converted_data;
	packet->size = converted_size;
	packet->pts = pts;

	int ret = avcodec_send_packet(decoder->codec_ctx, packet);
	av_packet_free(&packet);

	if (ret < 0) {
		CLOG_ERROR( "Error sending packet to decoder: %s (ret=%d)", av_err2str(ret), ret);
		bfree(converted_data);
		return false;
	}

	// CRITICAL: Drain ALL frames from the decoder, not just one
	// FFmpeg decoders can buffer multiple frames, and if we don't drain them,
	// frames will accumulate and cause memory issues/crashes
	int frames_decoded_this_packet = 0;
	bool success = false;
	
	while (true) {
		AVFrame *frame = av_frame_alloc();
		if (!frame) {
			CLOG_ERROR( "Failed to allocate AVFrame");
			break;
		}

		ret = avcodec_receive_frame(decoder->codec_ctx, frame);
		if (ret < 0) {
			if (ret == AVERROR(EAGAIN)) {
				// No more frames available right now - this is normal
				av_frame_free(&frame);
				break;
			} else if (ret == AVERROR_EOF) {
				// End of stream
				av_frame_free(&frame);
				break;
			} else {
				CLOG_ERROR( "Error receiving frame from decoder: %s (ret=%d)", av_err2str(ret), ret);
				av_frame_free(&frame);
				break;
			}
		}

		frames_decoded_this_packet++;
		success = true;

		// Convert frame to RGBA for OBS
		if (!decoder->sws_ctx) {
			decoder->sws_ctx = sws_getContext(
				frame->width, frame->height, (enum AVPixelFormat)frame->format,
				frame->width, frame->height, AV_PIX_FMT_RGBA,
				SWS_BILINEAR | SWS_FULL_CHR_H_INP | SWS_FULL_CHR_H_INT, NULL, NULL, NULL);
			if (!decoder->sws_ctx) {
				CLOG_ERROR( "Failed to create SWS context");
				av_frame_free(&frame);
				break;
			}
		}

		// Allocate buffer for RGBA data
		size_t rgba_size = frame->width * frame->height * 4; // 4 bytes per pixel for RGBA
		uint8_t *rgba_data = bzalloc(rgba_size);
		if (!rgba_data) {
			CLOG_ERROR( "Failed to allocate RGBA buffer: size=%zu", rgba_size);
			av_frame_free(&frame);
			break;
		}

		// Convert frame to RGBA
		uint8_t *dst_data[4] = {rgba_data, NULL, NULL, NULL};
		int dst_linesize[4] = {frame->width * 4, 0, 0, 0};

		int scale_ret = sws_scale(decoder->sws_ctx, (const uint8_t * const *)frame->data, frame->linesize,
		          0, frame->height, dst_data, dst_linesize);

		if (scale_ret < 0) {
			CLOG_ERROR( "sws_scale failed: %s (ret=%d)", av_err2str(scale_ret), scale_ret);
			bfree(rgba_data);
			av_frame_free(&frame);
			break;
		}

		// Use frame PTS if available, otherwise use packet PTS
		uint64_t frame_pts = frame->pts != AV_NOPTS_VALUE ? frame->pts : pts;
		
		// Validate frame data before output
		if (!rgba_data || frame->width == 0 || frame->height == 0) {
			CLOG_ERROR("Invalid frame data before output: rgba_data=%p width=%d height=%d",
			           rgba_data, frame->width, frame->height);
			bfree(rgba_data);
			av_frame_free(&frame);
			break;
		}
		
		// Output the decoded frame to OBS
		output_decoded_frame(context, rgba_data, frame->width, frame->height, frame_pts);

		av_frame_free(&frame);
	}

	bfree(converted_data);
	return success;
}

static void output_decoded_frame(struct hang_source *context, uint8_t *data, uint32_t width, uint32_t height, uint64_t pts_us)
{
	// IMPORTANT: This function is called while decoder_mutex is held.
	// We must ALWAYS unlock before returning to avoid deadlocks.
	// The caller (on_video_frame) expects the mutex to be unlocked after this returns.
	
	if (!context || !data) {
		if (data) {
			bfree(data);
		}
		// Unlock mutex before returning
		if (context) {
			pthread_mutex_unlock(&context->decoder_mutex);
		}
		return;
	}
	
	// Validate frame dimensions
	if (width == 0 || height == 0 || width > 7680 || height > 4320) {
		CLOG_ERROR("Invalid frame dimensions: %ux%u", width, height);
		bfree(data);
		pthread_mutex_unlock(&context->decoder_mutex);
		return;
	}

	// Copy all values we need while holding decoder_mutex to avoid race conditions
	bool is_active = context->active;
	obs_source_t *source = context->source;
	
	// Use PTS-based timestamps to ensure frames are displayed at the correct rate
	// PTS is in microseconds, convert to nanoseconds for OBS
	// This prevents OBS from accumulating frames when they arrive faster than display rate
	uint64_t obs_timestamp_ns = pts_us * 1000ULL;  // Convert microseconds to nanoseconds
	
	// Track first frame to establish baseline and ensure monotonic timestamps
	pthread_mutex_lock(&context->frame_mutex);
	if (!context->has_first_frame) {
		context->has_first_frame = true;
		context->first_frame_pts_us = pts_us;
		context->first_frame_obs_time_ns = os_gettime_ns();
		context->last_output_timestamp_ns = obs_timestamp_ns;
	} else {
		// Ensure timestamps are monotonically increasing
		// If PTS goes backwards (shouldn't happen but be defensive), adjust it
		if (obs_timestamp_ns <= context->last_output_timestamp_ns) {
			obs_timestamp_ns = context->last_output_timestamp_ns + 1;
		}
		context->last_output_timestamp_ns = obs_timestamp_ns;
	}
	pthread_mutex_unlock(&context->frame_mutex);
	
	// Now unlock decoder_mutex BEFORE calling OBS API to avoid deadlocks
	pthread_mutex_unlock(&context->decoder_mutex);
	
	// Check if source is still active after unlocking
	if (!is_active || !source) {
		bfree(data);
		return;
	}

	// Output frame to OBS using async video API
	// This is required for OBS_SOURCE_ASYNC_VIDEO sources
	struct obs_source_frame obs_frame = {0};
	obs_frame.data[0] = data;
	obs_frame.linesize[0] = width * 4;  // RGBA = 4 bytes per pixel
	obs_frame.width = width;
	obs_frame.height = height;
	obs_frame.format = VIDEO_FORMAT_RGBA;
	obs_frame.timestamp = obs_timestamp_ns;
	obs_frame.full_range = true;  // Set full range flag

	// Use the saved source pointer (copied while mutex was held)
	// OBS source pointers are stable and won't be freed until the source is destroyed,
	// and OBS will handle source destruction gracefully
	if (source && is_active) {
		obs_source_output_video(source, &obs_frame);
	}

	// OBS internally copies the frame data synchronously, so we can free our copy now
	bfree(data);
}
