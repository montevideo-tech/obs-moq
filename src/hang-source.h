/*
Hang MoQ Source for OBS
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

#pragma once

#include <obs-module.h>
#include <pthread.h>

// Forward declarations for decoder contexts
struct nvdec_decoder;
struct audio_decoder;

// Hang source context structure
struct hang_source {
	obs_source_t *source;

	// Settings
	char *url;
	char *broadcast_path;

	// MoQ resources (new API)
	int32_t origin_id;
	int32_t session_id;
	int32_t broadcast_id;
	int32_t catalog_consumer_id;
	int32_t video_track_id;
	int32_t audio_track_id;

	// Video state (for async video source)
	enum video_format format;

	// Audio state
	enum speaker_layout speakers;
	enum audio_format audio_format;
	uint32_t sample_rate;

	// Threading
	pthread_mutex_t frame_mutex;
	pthread_cond_t frame_cond;
	struct obs_source_frame **frame_queue;
	size_t frame_queue_len;
	size_t frame_queue_cap;

	pthread_mutex_t audio_mutex;
	pthread_cond_t audio_cond;
	struct obs_source_audio **audio_queue;
	size_t audio_queue_len;
	size_t audio_queue_cap;

	// Decoders
	struct nvdec_decoder *nvdec_context;
	struct audio_decoder *audio_decoder_context;
	pthread_mutex_t decoder_mutex; // Protects decoder access during callbacks

	// Timestamp tracking for frame synchronization
	uint64_t first_frame_pts_us;  // First frame's PTS from MoQ (microseconds)
	uint64_t first_frame_obs_time_ns;  // OBS time when first frame arrived (nanoseconds)
	uint64_t last_output_timestamp_ns;  // Last timestamp we sent to OBS (for monotonicity check)
	bool has_first_frame;  // Whether we've received the first frame

	// Running state
	bool active;
};

// Declare the hang source info structure
extern struct obs_source_info hang_source_info;
