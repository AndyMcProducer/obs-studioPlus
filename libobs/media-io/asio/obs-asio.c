// The implementation includes ASIO SDK headers only when available.
#ifdef _WIN32
#include <windows.h>
#endif

#include "obs-asio.h"

// Minimal stub implementations to allow the frontend to build when ASIO
// SDK headers are not present. When the SDK is available, these functions
// will be replaced with real implementations that include "asio.h" and
// "asiodrivers.h" and query the installed drivers.

#define MAX_ASIO_DEVICES 0

int obs_asio_get_device_count(void)
{
#ifdef _WIN32
    (void)0; // placeholder
#endif
    return 0;
}

const char *obs_asio_get_device_name(int index)
{
    (void)index;
    return NULL;
}

bool obs_asio_open_control_panel(const char *device_name)
{
    (void)device_name;
    return false;
}

void obs_asio_set_selected_device(const char *device_name)
{
    (void)device_name;
}

bool obs_asio_initialize(struct obs_asio_context *ctx, long sample_rate, long buffer_size, int num_channels)
{
    (void)ctx; (void)sample_rate; (void)buffer_size; (void)num_channels;
    return false;
}

void obs_asio_set_audio_callback(struct obs_asio_context *ctx, obs_asio_audio_callback_t cb, void *user_data)
{
    (void)ctx; (void)cb; (void)user_data;
}

void obs_asio_shutdown(struct obs_asio_context *ctx)
{
    (void)ctx;
}

bool obs_asio_start(struct obs_asio_context *ctx)
{
    (void)ctx;
    return false;
}

void obs_asio_stop(struct obs_asio_context *ctx)
{
    (void)ctx;
}
// End of stubs. Real ASIO SDK implementation will be added and compiled
// conditionally when the ASIO SDK is available and CMake is configured to
// include it.
