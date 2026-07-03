/* Minimal ASIO -> libobs bridge implementation
 * - Allocates a per-channel ring buffer
 * - Registers an obs_asio audio callback to copy ASIO frames into the ring
 * - Provides an engine callback that reads from the ring into libobs buffers
 *
 * This is intentionally a simple prototype: it uses a mutex to synchronize
 * between the ASIO callback and the audio engine. It assumes the ASIO
 * sample rate matches the libobs audio sample rate (no resampling).
 */

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <inttypes.h>

#include "obs-asio-bridge.h"
#include "obs-asio.h"
#include "obs.h"
#include "obs-internal.h"

#define ASIO_GLOBAL_ROUTE_COUNT 6
#define ASIO_MAX_TAPS 32

struct obs_asio_bridge_tap {
    bool output;
    int left;
    int right;
    obs_asio_bridge_tap_callback_t callback;
    void *user_data;
};

static pthread_mutex_t tap_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct obs_asio_bridge_tap *taps[ASIO_MAX_TAPS] = {0};

static struct {
    struct obs_asio_context *ctx;
    bool initialized;
    bool started;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t output_channels;
    uint32_t asio_buffer_frames;
    size_t ring_frames; /* frames per channel */
    float *ringbuf;     /* size = channels * ring_frames */
    float *output_ringbuf; /* size = 2 * ring_frames */
    pthread_mutex_t mutex;
    uint64_t write_index; /* absolute frame count written */
    uint64_t read_index;  /* absolute frame count read */
    uint64_t output_write_index;
    uint64_t output_read_index;
    struct obs_asio_channel_pair input_pairs[ASIO_GLOBAL_ROUTE_COUNT];
    struct obs_asio_channel_pair monitor_pair;
} bridge = {0};

static uint64_t get_audio_block_timestamp(int num_frames)
{
    uint64_t now = os_gettime_ns();

    if (!bridge.sample_rate || num_frames <= 0)
        return now;

    uint64_t duration = util_mul_div64((uint64_t)num_frames, 1000000000ULL, bridge.sample_rate);
    return now > duration ? now - duration : now;
}

static void fire_asio_taps(bool output, float **channels, int num_channels, int num_frames)
{
    uint64_t timestamp = get_audio_block_timestamp(num_frames);

    pthread_mutex_lock(&tap_mutex);

    for (size_t i = 0; i < ASIO_MAX_TAPS; ++i) {
        struct obs_asio_bridge_tap *tap = taps[i];
        const float *left;
        const float *right = NULL;

        if (!tap || tap->output != output || !tap->callback)
            continue;
        if (tap->left < 0 || tap->left >= num_channels)
            continue;

        left = channels[tap->left];
        if (!left)
            continue;

        if (tap->right >= 0 && tap->right < num_channels && tap->right != tap->left)
            right = channels[tap->right];

        tap->callback(left, right, num_frames, bridge.sample_rate, timestamp, tap->user_data);
    }

    pthread_mutex_unlock(&tap_mutex);
}

static inline void execute_asio_audio_tasks(void)
{
    struct obs_core_audio *audio = &obs->audio;
    bool tasks_remaining = true;

    while (tasks_remaining) {
        pthread_mutex_lock(&audio->task_mutex);
        if (audio->tasks.size) {
            struct obs_task_info info;
            deque_pop_front(&audio->tasks, &info, sizeof(info));
            info.task(info.param);
        }
        tasks_remaining = !!audio->tasks.size;
        pthread_mutex_unlock(&audio->task_mutex);
    }
}

EXPORT void obs_asio_bridge_set_input_pairs(const struct obs_asio_channel_pair *pairs, uint32_t count)
{
    if (bridge.initialized)
        pthread_mutex_lock(&bridge.mutex);

    for (uint32_t i = 0; i < ASIO_GLOBAL_ROUTE_COUNT; ++i) {
        if (pairs && i < count) {
            bridge.input_pairs[i] = pairs[i];
        } else {
            bridge.input_pairs[i].left = -1;
            bridge.input_pairs[i].right = -1;
        }
    }

    if (bridge.initialized)
        pthread_mutex_unlock(&bridge.mutex);
}

EXPORT void obs_asio_bridge_set_monitor_pair(int left, int right)
{
    if (bridge.initialized)
        pthread_mutex_lock(&bridge.mutex);

    bridge.monitor_pair.left = left;
    bridge.monitor_pair.right = right;
    if (left < 0 || right < 0)
	bridge.output_read_index = bridge.output_write_index;

    if (bridge.initialized)
        pthread_mutex_unlock(&bridge.mutex);
}

EXPORT struct obs_asio_bridge_tap *obs_asio_bridge_create_tap(bool output,
						      obs_asio_bridge_tap_callback_t callback,
						      void *user_data)
{
    struct obs_asio_bridge_tap *tap = (struct obs_asio_bridge_tap *)bmalloc(sizeof(*tap));
    struct obs_asio_bridge_tap *result = NULL;

    if (!tap)
        return NULL;

    memset(tap, 0, sizeof(*tap));
    tap->output = output;
    tap->left = -1;
    tap->right = -1;
    tap->callback = callback;
    tap->user_data = user_data;

    pthread_mutex_lock(&tap_mutex);

    for (size_t i = 0; i < ASIO_MAX_TAPS; ++i) {
        if (taps[i])
            continue;

        taps[i] = tap;
        result = tap;
        break;
    }

    pthread_mutex_unlock(&tap_mutex);

    if (!result)
        bfree(tap);

    return result;
}

EXPORT void obs_asio_bridge_destroy_tap(struct obs_asio_bridge_tap *tap)
{
    if (!tap)
        return;

    pthread_mutex_lock(&tap_mutex);

    for (size_t i = 0; i < ASIO_MAX_TAPS; ++i) {
        if (taps[i] != tap)
            continue;

        taps[i] = NULL;
        break;
    }

    pthread_mutex_unlock(&tap_mutex);
    bfree(tap);
}

EXPORT void obs_asio_bridge_tap_set_channels(struct obs_asio_bridge_tap *tap, int left, int right)
{
    if (!tap)
        return;

    pthread_mutex_lock(&tap_mutex);
    tap->left = left;
    tap->right = right;
    pthread_mutex_unlock(&tap_mutex);
}

/* ASIO-side callback: receives planar float buffers and copies into ring */
static void asio_bridge_asio_cb(float **input, int num_channels, int num_frames, void *user_data)
{
    (void)user_data;
    if (!bridge.initialized || !bridge.ringbuf)
        return;

    pthread_mutex_lock(&bridge.mutex);

    uint64_t w = bridge.write_index;
    size_t rf = bridge.ring_frames;
    uint32_t chs = bridge.channels;

    for (uint32_t ch = 0; ch < (uint32_t)num_channels && ch < chs; ++ch) {
        float *src = input[ch];
        float *dst = bridge.ringbuf + (size_t)ch * rf;

        size_t idx = (size_t)(w % rf);
        if (idx + (size_t)num_frames <= rf) {
            memcpy(dst + idx, src, sizeof(float) * (size_t)num_frames);
        } else {
            size_t first = rf - idx;
            memcpy(dst + idx, src, sizeof(float) * first);
            memcpy(dst, src + first, sizeof(float) * ((size_t)num_frames - first));
        }
    }

    bridge.write_index = w + (uint64_t)num_frames;
    pthread_mutex_unlock(&bridge.mutex);

    fire_asio_taps(false, input, num_channels, num_frames);
}

static void asio_bridge_output_cb(float **output, int num_channels, int num_frames, void *user_data)
{
    (void)user_data;
    if (!bridge.initialized || !output)
        return;

    pthread_mutex_lock(&bridge.mutex);

    for (int ch = 0; ch < num_channels; ++ch) {
        if (output[ch])
            memset(output[ch], 0, sizeof(float) * (size_t)num_frames);
    }

    if (!bridge.output_ringbuf || bridge.monitor_pair.left < 0 || bridge.monitor_pair.left >= num_channels) {
	bridge.output_read_index = bridge.output_write_index;
        pthread_mutex_unlock(&bridge.mutex);
	fire_asio_taps(true, output, num_channels, num_frames);
        return;
    }

    uint64_t available = bridge.output_write_index - bridge.output_read_index;
    uint64_t requested = (uint64_t)num_frames;
    size_t tocopy = (size_t)(available >= requested ? requested : available);
    size_t idx = (size_t)(bridge.output_read_index % bridge.ring_frames);

    if (tocopy > 0) {
        float *left_dst = output[bridge.monitor_pair.left];
        float *right_dst = (bridge.monitor_pair.right >= 0 && bridge.monitor_pair.right < num_channels)
			       ? output[bridge.monitor_pair.right]
                   : NULL;

        if (left_dst) {
            if (idx + tocopy <= bridge.ring_frames) {
                memcpy(left_dst, bridge.output_ringbuf + idx, sizeof(float) * tocopy);
            } else {
                size_t first = bridge.ring_frames - idx;
                memcpy(left_dst, bridge.output_ringbuf + idx, sizeof(float) * first);
                memcpy(left_dst + first, bridge.output_ringbuf, sizeof(float) * (tocopy - first));
            }
        }

        if (right_dst) {
            float *right_src = bridge.output_ringbuf + bridge.ring_frames;
            if (idx + tocopy <= bridge.ring_frames) {
                memcpy(right_dst, right_src + idx, sizeof(float) * tocopy);
            } else {
                size_t first = bridge.ring_frames - idx;
                memcpy(right_dst, right_src + idx, sizeof(float) * first);
                memcpy(right_dst + first, right_src, sizeof(float) * (tocopy - first));
            }
        }
    }

    bridge.output_read_index += (uint64_t)num_frames;
    pthread_mutex_unlock(&bridge.mutex);

    fire_asio_taps(true, output, num_channels, num_frames);
}

EXPORT bool obs_asio_bridge_setup(uint32_t sample_rate, uint32_t channels, uint32_t asio_buffer_frames)
{
    if (bridge.initialized)
        return true;

    if (sample_rate == 0)
        return false;

    bridge.sample_rate = sample_rate;
    bridge.channels = channels;
    bridge.asio_buffer_frames = asio_buffer_frames ? asio_buffer_frames : AUDIO_OUTPUT_FRAMES;

    /* Ring buffer: 2 seconds default, at least a few engine blocks */
    size_t seconds = 2;
    size_t rf = (size_t)sample_rate * seconds;
    if (rf < AUDIO_OUTPUT_FRAMES * 4)
        rf = AUDIO_OUTPUT_FRAMES * 4;
    bridge.ring_frames = rf;

    bridge.ringbuf = (float *)bmalloc(sizeof(float) * (size_t)channels * bridge.ring_frames);
    if (!bridge.ringbuf)
        return false;
    memset(bridge.ringbuf, 0, sizeof(float) * (size_t)channels * bridge.ring_frames);

    if (pthread_mutex_init(&bridge.mutex, NULL) != 0) {
        bfree(bridge.ringbuf);
        bridge.ringbuf = NULL;
        return false;
    }

    bridge.ctx = (struct obs_asio_context *)bmalloc(sizeof(struct obs_asio_context));
    if (!bridge.ctx) {
        pthread_mutex_destroy(&bridge.mutex);
        bfree(bridge.ringbuf);
        bridge.ringbuf = NULL;
        return false;
    }
    memset(bridge.ctx, 0, sizeof(struct obs_asio_context));

    /* Initialize ASIO driver (backend will create buffers). Capture all inputs so selectors can map any pair. */
    if (!obs_asio_initialize(bridge.ctx, (long)sample_rate, (long)bridge.asio_buffer_frames, 0)) {
        bfree(bridge.ctx);
        bridge.ctx = NULL;
        pthread_mutex_destroy(&bridge.mutex);
        bfree(bridge.ringbuf);
        bridge.ringbuf = NULL;
        return false;
    }

    bridge.channels = (uint32_t)bridge.ctx->num_input_channels;
    bridge.output_channels = (uint32_t)bridge.ctx->num_output_channels;

    bfree(bridge.ringbuf);
    bridge.ringbuf = (float *)bmalloc(sizeof(float) * (size_t)bridge.channels * bridge.ring_frames);
    if (!bridge.ringbuf) {
        obs_asio_shutdown(bridge.ctx);
        bfree(bridge.ctx);
        bridge.ctx = NULL;
        pthread_mutex_destroy(&bridge.mutex);
        return false;
    }
    memset(bridge.ringbuf, 0, sizeof(float) * (size_t)bridge.channels * bridge.ring_frames);

    bridge.output_ringbuf = (float *)bmalloc(sizeof(float) * 2 * bridge.ring_frames);
    if (!bridge.output_ringbuf) {
        obs_asio_shutdown(bridge.ctx);
        bfree(bridge.ctx);
        bridge.ctx = NULL;
        pthread_mutex_destroy(&bridge.mutex);
        bfree(bridge.ringbuf);
        bridge.ringbuf = NULL;
        return false;
    }
    memset(bridge.output_ringbuf, 0, sizeof(float) * 2 * bridge.ring_frames);

    /* Register callback to copy ASIO frames into ring */
    obs_asio_set_audio_callback(bridge.ctx, asio_bridge_asio_cb, NULL);
    obs_asio_set_output_callback(bridge.ctx, asio_bridge_output_cb, NULL);

    bridge.write_index = 0;
    bridge.read_index = 0;
    bridge.output_write_index = 0;
    bridge.output_read_index = 0;
    bridge.initialized = true;
    bridge.started = false;

    return true;
}

EXPORT bool obs_asio_bridge_start(void)
{
    if (!bridge.initialized || bridge.started)
        return bridge.initialized;

    if (!bridge.ctx)
        return false;

    if (!obs_asio_start(bridge.ctx))
        return false;

    bridge.started = true;
    return true;
}

EXPORT void obs_asio_bridge_stop(void)
{
    if (!bridge.initialized || !bridge.started)
        return;

    if (bridge.ctx)
        obs_asio_stop(bridge.ctx);

    bridge.started = false;
}

EXPORT void obs_asio_bridge_shutdown(void)
{
    if (!bridge.initialized)
        return;

    /* Stop driver */
    if (bridge.started && bridge.ctx)
        obs_asio_stop(bridge.ctx);

    /* Shutdown driver and free context */
    if (bridge.ctx)
        obs_asio_shutdown(bridge.ctx);

    if (bridge.ctx) {
        bfree(bridge.ctx);
        bridge.ctx = NULL;
    }

    pthread_mutex_lock(&bridge.mutex);
    if (bridge.ringbuf) {
        bfree(bridge.ringbuf);
        bridge.ringbuf = NULL;
    }
    if (bridge.output_ringbuf) {
        bfree(bridge.output_ringbuf);
        bridge.output_ringbuf = NULL;
    }
    bridge.write_index = 0;
    bridge.read_index = 0;
    bridge.output_write_index = 0;
    bridge.output_read_index = 0;
    pthread_mutex_unlock(&bridge.mutex);

    pthread_mutex_destroy(&bridge.mutex);
    bridge.initialized = false;
    bridge.started = false;
    bridge.channels = 0;
    bridge.output_channels = 0;
}

/* Engine callback called from the audio thread to fetch AUDIO_OUTPUT_FRAMES frames */
EXPORT bool asio_bridge_engine_callback(void *param, uint64_t start_ts, uint64_t end_ts, uint64_t *new_ts,
                                       uint32_t active_mixers, struct audio_output_data *mixes)
{
    (void)param;
    (void)start_ts;

    if (!bridge.initialized || !bridge.ringbuf)
        return false;

    const size_t frames = AUDIO_OUTPUT_FRAMES;
    size_t rf = bridge.ring_frames;
    uint32_t chs = bridge.channels;

    pthread_mutex_lock(&bridge.mutex);

    uint64_t available = bridge.write_index - bridge.read_index;
    size_t tocopy = (size_t)(available >= (uint64_t)frames ? frames : available);
    size_t idx = (size_t)(bridge.read_index % rf);

    struct obs_asio_channel_pair routes[ASIO_GLOBAL_ROUTE_COUNT];
    bool has_route = false;
    for (size_t i = 0; i < ASIO_GLOBAL_ROUTE_COUNT; ++i) {
        routes[i] = bridge.input_pairs[i];
        if (routes[i].left >= 0 && routes[i].left < (int)chs)
            has_route = true;
    }

    if (!has_route && chs > 0) {
        routes[0].left = 0;
        routes[0].right = chs > 1 ? 1 : 0;
        has_route = true;
    }

    for (size_t mix_idx = 0; mix_idx < MAX_AUDIO_MIXES; ++mix_idx) {
        if ((active_mixers & (1 << mix_idx)) == 0)
            continue;

        float *left_out = mixes[mix_idx].data[0];
        float *right_out = mixes[mix_idx].data[1] ? mixes[mix_idx].data[1] : left_out;
        if (!left_out)
            continue;

        for (size_t route_idx = 0; route_idx < ASIO_GLOBAL_ROUTE_COUNT; ++route_idx) {
            int left_channel = routes[route_idx].left;
            int right_channel = routes[route_idx].right;

            if (left_channel < 0 || left_channel >= (int)chs)
                continue;
            if (right_channel < 0 || right_channel >= (int)chs)
                right_channel = left_channel;

            const float *left_src = bridge.ringbuf + (size_t)left_channel * rf;
            const float *right_src = bridge.ringbuf + (size_t)right_channel * rf;

            for (size_t frame = 0; frame < tocopy; ++frame) {
                size_t sample_idx = idx + frame;
                if (sample_idx >= rf)
                    sample_idx -= rf;
                left_out[frame] += left_src[sample_idx];
                right_out[frame] += right_src[sample_idx];
            }
        }
    }

    if (bridge.output_ringbuf) {
        float *monitor_left = (active_mixers & 1) ? mixes[0].data[0] : NULL;
        float *monitor_right = (active_mixers & 1) ? mixes[0].data[1] : NULL;
        float *out_left = bridge.output_ringbuf;
        float *out_right = bridge.output_ringbuf + rf;
        size_t out_idx = (size_t)(bridge.output_write_index % rf);

        if (out_idx + frames <= rf) {
            if (monitor_left) {
                memcpy(out_left + out_idx, monitor_left, sizeof(float) * frames);
            } else {
                memset(out_left + out_idx, 0, sizeof(float) * frames);
            }

            if (monitor_right) {
                memcpy(out_right + out_idx, monitor_right, sizeof(float) * frames);
            } else if (monitor_left) {
                memcpy(out_right + out_idx, monitor_left, sizeof(float) * frames);
            } else {
                memset(out_right + out_idx, 0, sizeof(float) * frames);
            }
        } else {
            size_t first = rf - out_idx;
            size_t second = frames - first;

            if (monitor_left) {
                memcpy(out_left + out_idx, monitor_left, sizeof(float) * first);
                memcpy(out_left, monitor_left + first, sizeof(float) * second);
            } else {
                memset(out_left + out_idx, 0, sizeof(float) * first);
                memset(out_left, 0, sizeof(float) * second);
            }

            if (monitor_right) {
                memcpy(out_right + out_idx, monitor_right, sizeof(float) * first);
                memcpy(out_right, monitor_right + first, sizeof(float) * second);
            } else if (monitor_left) {
                memcpy(out_right + out_idx, monitor_left, sizeof(float) * first);
                memcpy(out_right, monitor_left + first, sizeof(float) * second);
            } else {
                memset(out_right + out_idx, 0, sizeof(float) * first);
                memset(out_right, 0, sizeof(float) * second);
            }
        }

        bridge.output_write_index += (uint64_t)frames;
    }

    /* Advance read pointer by the number of frames we consumed */
    bridge.read_index += frames;

    pthread_mutex_unlock(&bridge.mutex);

    if (new_ts)
        *new_ts = end_ts;

    execute_asio_audio_tasks();

    return true;
}
