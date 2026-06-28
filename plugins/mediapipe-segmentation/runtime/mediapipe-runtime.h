#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Runtime ABI consumed by the OBS filter.
 *
 * A MediaPipe/LiteRT implementation should export these symbols from
 * obs-mediapipe-runtime.dll. The runtime receives full-size RGBA frames and
 * writes one 8-bit foreground probability per output pixel.
 */
typedef void *obs_mediapipe_runtime_t;

typedef obs_mediapipe_runtime_t (*obs_mediapipe_runtime_create_t)(const char *model_path, uint32_t width,
								  uint32_t height, char *error,
								  size_t error_size);
typedef void (*obs_mediapipe_runtime_destroy_t)(obs_mediapipe_runtime_t runtime);
typedef bool (*obs_mediapipe_runtime_process_rgba_t)(obs_mediapipe_runtime_t runtime, const uint8_t *rgba,
						     uint32_t width, uint32_t height, uint32_t rgba_linesize,
						     uint8_t *mask, uint32_t mask_linesize);
typedef uint32_t (*obs_mediapipe_runtime_version_t)(void);

#ifdef __cplusplus
}
#endif
