/* ASIO <-> libobs bridge
 * Simple ring-buffer bridge to forward ASIO float planar buffers into
 * libobs audio_input (engine) callback. Prototype / lightweight API used
 * by obs.c to initialize/start/stop the bridge when ASIO is selected.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "../audio-io.h"

#ifdef __cplusplus
extern "C" {
#endif

struct obs_asio_channel_pair {
    int left;
    int right;
};

struct obs_asio_bridge_tap;

typedef void (*obs_asio_bridge_tap_callback_t)(const float *left, const float *right, int num_frames,
					       uint32_t sample_rate, uint64_t timestamp,
					       void *user_data);

/* Setup the bridge. Does not start ASIO streaming. */
EXPORT bool obs_asio_bridge_setup(uint32_t sample_rate, uint32_t channels, uint32_t asio_buffer_frames);

/* Update the ASIO input and monitor routing. */
EXPORT void obs_asio_bridge_set_input_pairs(const struct obs_asio_channel_pair *pairs, uint32_t count);
EXPORT void obs_asio_bridge_set_monitor_pair(int left, int right);

/* Register live ASIO taps for source capture. */
EXPORT struct obs_asio_bridge_tap *obs_asio_bridge_create_tap(bool output,
                              obs_asio_bridge_tap_callback_t callback,
                              void *user_data);
EXPORT void obs_asio_bridge_destroy_tap(struct obs_asio_bridge_tap *tap);
EXPORT void obs_asio_bridge_tap_set_channels(struct obs_asio_bridge_tap *tap, int left, int right);

/* Start / stop ASIO streaming (after engine audio is started). */
EXPORT bool obs_asio_bridge_start(void);
EXPORT void obs_asio_bridge_stop(void);

/* Tear down the bridge and free resources. Safe to call multiple times. */
EXPORT void obs_asio_bridge_shutdown(void);

/* Engine callback to read audio from the ASIO ring buffer into libobs.
 * This function matches audio_input_callback_t and is intended to be
 * assigned to `audio_output_info::input_callback` when ASIO is active.
 */
EXPORT bool asio_bridge_engine_callback(void *param, uint64_t start_ts, uint64_t end_ts, uint64_t *new_ts,
                                       uint32_t active_mixers, struct audio_output_data *mixes);

#ifdef __cplusplus
}
#endif
