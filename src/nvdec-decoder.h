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

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <obs-module.h>

struct hang_source;

// Video decoder functions (software decoding only)
// codec: codec string (e.g. "avc1.42E01E", "hev1.1.6.L93.B0", "av01.0.04M.08")
// description: codec-specific extradata (e.g. AVCC for H.264), NULL for annex-b format
// description_len: length of description, 0 if description is NULL
bool nvdec_decoder_init(struct hang_source *context, const char *codec, size_t codec_len,
                        const uint8_t *description, size_t description_len);
void nvdec_decoder_destroy(struct hang_source *context);
bool nvdec_decoder_decode(struct hang_source *context, const uint8_t *data, size_t size, uint64_t pts, bool keyframe);
