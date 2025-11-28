#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/**
 * # Safety
 *
 * The caller must ensure that:
 * - `c_server_url` and `c_path` are valid null-terminated C strings
 * - The pointers remain valid for the duration of this function call
 */
void hang_start_from_c(const char *c_server_url, const char *c_path, const char *_c_profile);

void hang_stop_from_c(void);

/**
 * # Safety
 *
 * The caller must ensure that:
 * - `data` points to a valid buffer of at least `size` bytes
 * - The buffer remains valid for the duration of this function call
 */
void hang_write_video_packet_from_c(const uint8_t *data,
                                    uintptr_t size,
                                    int32_t keyframe,
                                    uint64_t dts);

/**
 * # Safety
 *
 * The caller must ensure that:
 * - `data` points to a valid buffer of at least `size` bytes
 * - The buffer remains valid for the duration of this function call
 */
void hang_write_audio_packet_from_c(const uint8_t *data, uintptr_t size, uint64_t dts);
