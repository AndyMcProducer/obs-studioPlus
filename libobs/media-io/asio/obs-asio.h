// OBS Studio ASIO backend implementation stub
// Copyright (C) 2026
// This file provides the basic structure for integrating the Steinberg ASIO SDK into OBS Studio's audio backend.
// Place the ASIO SDK headers and libs in C:/SDKs/ASIO as per user instructions.

#ifdef _WIN32
#include <windows.h>
// Do NOT include ASIO SDK headers here. The implementation file (obs-asio.c)
// includes the ASIO SDK headers so that users who do not have the SDK are not
// required to add its include path to the global build. This header exposes a
// plain C API that does not depend on ASIO types.
#endif

#include <stdint.h>
#include <stdbool.h>
#include "../audio-io.h"

#ifdef __cplusplus
extern "C" {
#endif


// Device enumeration
EXPORT int obs_asio_get_device_count(void);
EXPORT const char *obs_asio_get_device_name(int index);
EXPORT int obs_asio_get_input_channel_count(void);
EXPORT const char *obs_asio_get_input_channel_name(int index);
EXPORT int obs_asio_get_output_channel_count(void);
EXPORT const char *obs_asio_get_output_channel_name(int index);
EXPORT void obs_asio_set_selected_device(const char *device_name);

// Open control panel for a device
EXPORT bool obs_asio_open_control_panel(const char *device_name);


typedef void (*obs_asio_audio_callback_t)(float **input, int num_channels, int num_frames, void *user_data);
typedef void (*obs_asio_audio_output_callback_t)(float **output, int num_channels, int num_frames, void *user_data);

struct obs_asio_context {
    void *driver;
    long sample_rate;
    long buffer_size;
    int num_channels;
    int num_input_channels;
    int num_output_channels;
    obs_asio_audio_callback_t audio_cb;
    void *audio_cb_data;
    obs_asio_audio_output_callback_t output_cb;
    void *output_cb_data;
};

EXPORT bool obs_asio_initialize(struct obs_asio_context *ctx, long sample_rate, long buffer_size, int num_channels);
EXPORT void obs_asio_set_audio_callback(struct obs_asio_context *ctx, obs_asio_audio_callback_t cb, void *user_data);
EXPORT void obs_asio_set_output_callback(struct obs_asio_context *ctx, obs_asio_audio_output_callback_t cb,
					 void *user_data);
EXPORT void obs_asio_shutdown(struct obs_asio_context *ctx);
EXPORT bool obs_asio_start(struct obs_asio_context *ctx);
EXPORT void obs_asio_stop(struct obs_asio_context *ctx);

#ifdef __cplusplus
}
#endif
