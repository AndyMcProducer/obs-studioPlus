/*
 * Copyright (c) 2015 John R. Bradley <jrb@turrettech.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <math.h>

#include <obs-module.h>
#include <media-io/audio-io.h>
#include <media-io/audio-resampler.h>
#include <util/darray.h>
#include <util/threading.h>
#include <util/platform.h>
#include <util/dstr.h>

#include "obs-ffmpeg-compat.h"
#include "obs-ffmpeg-formats.h"

#include <media-playback/media-playback.h>

#define FF_LOG_S(source, level, format, ...) \
	blog(level, "[Media Source '%s']: " format, obs_source_get_name(source), ##__VA_ARGS__)
#define FF_BLOG(level, format, ...) FF_LOG_S(s->source, level, format, ##__VA_ARGS__)

#define S_PLAYLIST "playlist"
#define FFMPEG_PLAYLIST_SOURCE_ID "ffmpeg_source"
#define FFMPEG_MEDIA_SOURCE_ID "ffmpeg_media_source"

enum ffmpeg_transition_mode {
	FFMPEG_TRANSITION_CUT,
	FFMPEG_TRANSITION_FADE,
	FFMPEG_TRANSITION_CROSSFADE,
};

struct ffmpeg_source;

struct ffmpeg_audio_packet {
	float *data[MAX_AUDIO_CHANNELS];
	size_t capacity_frames;
	uint32_t frames;
	uint64_t timestamp;
	bool valid;
};

struct ffmpeg_audio_stream_state {
	audio_resampler_t *resampler;
	struct resample_info input_info;
	bool input_info_valid;
	bool resampler_failed;
	struct ffmpeg_audio_packet packet;
};

struct ffmpeg_media_callback {
	struct ffmpeg_source *source;
	bool transition;
	struct ffmpeg_audio_stream_state audio;
};

typedef DARRAY(char *) media_playlist_t;

struct ffmpeg_source {
	media_playback_t *media;
	media_playback_t *transition_media;
	bool destroy_media;
	bool destroy_transition_media;

	enum video_range_type range;
	bool is_linear_alpha;
	obs_source_t *source;
	obs_hotkey_id hotkey;

	char *input;
	char *active_input;
	char *input_format;
	char *ffmpeg_options;
	media_playlist_t playlist;
	size_t playlist_index;
	size_t pending_playlist_index;
	int buffering_mb;
	int speed_percent;
	bool is_looping;
	bool configured_is_local_file;
	bool is_local_file;
	bool is_hw_decoding;
	bool full_decode;
	bool is_clear_on_media_end;
	bool restart_on_activate;
	bool close_when_inactive;
	bool seekable;
	bool is_stinger;
	bool is_track_matte;
	bool log_changes;
	bool playlist_source;
	int transition_ms;
	enum ffmpeg_transition_mode transition_mode;
	bool transition_active;
	bool transition_midpoint_pending;
	uint64_t transition_start_ns;
	uint8_t *transition_audio_buffer[MAX_AV_PLANES];
	size_t transition_audio_buffer_size;

	pthread_t reconnect_thread;
	pthread_mutex_t reconnect_mutex;
	bool reconnect_thread_valid;
	os_event_t *reconnect_stop_event;
	volatile bool reconnecting;
	int reconnect_delay_sec;
	bool suppress_stop_callback;
	bool suppress_transition_stop_callback;
	bool playlist_switch_pending;
	uint64_t transition_serial;
	float *mix_audio_buffer[MAX_AUDIO_CHANNELS];
	size_t mix_audio_capacity_frames;
	struct ffmpeg_media_callback callback_a;
	struct ffmpeg_media_callback callback_b;
	struct ffmpeg_media_callback *current_callback;
	struct ffmpeg_media_callback *transition_callback;

	enum obs_media_state state;
	obs_hotkey_pair_id play_pause_hotkey;
	obs_hotkey_id stop_hotkey;
};

static inline bool ffmpeg_source_is_playlist_source_id(const char *id)
{
	return id && strcmp(id, FFMPEG_PLAYLIST_SOURCE_ID) == 0;
}

static inline bool ffmpeg_source_supports_playlist(const struct ffmpeg_source *s)
{
	return s->playlist_source;
}

static inline struct ffmpeg_media_callback *ffmpeg_source_other_callback(struct ffmpeg_source *s,
								 struct ffmpeg_media_callback *callback)
{
	return callback == &s->callback_a ? &s->callback_b : &s->callback_a;
}

static inline enum ffmpeg_transition_mode ffmpeg_source_clamp_transition_mode(int mode)
{
	return mode < FFMPEG_TRANSITION_CUT || mode > FFMPEG_TRANSITION_CROSSFADE
		       ? FFMPEG_TRANSITION_CROSSFADE
		       : (enum ffmpeg_transition_mode)mode;
}

static inline uint64_t ffmpeg_source_transition_duration_ns(const struct ffmpeg_source *s)
{
	return s->transition_ms > 0 ? (uint64_t)s->transition_ms * 1000000ULL : 0;
}

static bool ffmpeg_source_is_fade_mode(const struct ffmpeg_source *s)
{
	return s->transition_mode == FFMPEG_TRANSITION_FADE && s->transition_ms > 0;
}

static bool ffmpeg_source_is_crossfade_mode(const struct ffmpeg_source *s)
{
	return s->transition_mode == FFMPEG_TRANSITION_CROSSFADE && s->transition_ms > 0;
}

static void ffmpeg_source_reset_transition(struct ffmpeg_source *s)
{
	s->transition_active = false;
	s->transition_midpoint_pending = false;
	s->transition_start_ns = 0;
}

static void ffmpeg_source_free_transition_audio(struct ffmpeg_source *s)
{
	for (size_t i = 0; i < MAX_AV_PLANES; i++) {
		bfree(s->transition_audio_buffer[i]);
		s->transition_audio_buffer[i] = NULL;
	}

	s->transition_audio_buffer_size = 0;
}

static void ffmpeg_source_start_crossfade_transition(struct ffmpeg_source *s, size_t index);

static void ffmpeg_source_start_fade_transition(struct ffmpeg_source *s, size_t index, bool delay_switch)
{
	uint64_t start_ns = os_gettime_ns();
	uint64_t duration_ns = ffmpeg_source_transition_duration_ns(s);

	s->pending_playlist_index = index;
	s->transition_active = true;
	s->transition_midpoint_pending = delay_switch;
	s->transition_start_ns = (delay_switch || !duration_ns) ? start_ns : start_ns - duration_ns / 2;
	if (s->transition_start_ns > start_ns)
		s->transition_start_ns = 0;
}

static bool ffmpeg_source_should_delay_switch(const struct ffmpeg_source *s)
{
	return ffmpeg_source_is_fade_mode(s) && s->media && s->state == OBS_MEDIA_STATE_PLAYING;
}

static float ffmpeg_source_get_fade_gain(const struct ffmpeg_source *s, uint64_t timestamp)
{
	uint64_t duration_ns;
	uint64_t elapsed;
	float progress;

	if (!s->transition_active || !ffmpeg_source_is_fade_mode(s) || !s->transition_start_ns)
		return 1.0f;

	duration_ns = ffmpeg_source_transition_duration_ns(s);
	if (!duration_ns)
		return 1.0f;

	elapsed = timestamp > s->transition_start_ns ? timestamp - s->transition_start_ns : 0;
	if (elapsed >= duration_ns)
		return 1.0f;

	progress = (float)elapsed / (float)duration_ns;
	return progress < 0.5f ? 1.0f - progress * 2.0f : (progress - 0.5f) * 2.0f;
}

static bool ffmpeg_source_get_audio_transition_gains(const struct ffmpeg_source *s, const struct obs_source_audio *a,
					      float *gains)
{
	bool varying = false;
	float prev_gain = ffmpeg_source_get_fade_gain(s, a->timestamp);

	gains[0] = prev_gain;
	for (uint32_t frame = 1; frame < a->frames; frame++) {
		uint64_t frame_ts = a->timestamp + audio_frames_to_ns(a->samples_per_sec, frame);
		float gain = ffmpeg_source_get_fade_gain(s, frame_ts);

		gains[frame] = gain;
		varying |= fabsf(gain - prev_gain) > 0.0001f;
		prev_gain = gain;
	}

	return varying || fabsf(gains[0] - 1.0f) > 0.0001f;
}

static inline uint8_t clamp_u8_sample(int value)
{
	if (value > 255)
		return 255;
	if (value < 0)
		return 0;
	return (uint8_t)value;
}

static inline int16_t clamp_s16_sample(int value)
{
	if (value > INT16_MAX)
		return INT16_MAX;
	if (value < INT16_MIN)
		return INT16_MIN;
	return (int16_t)value;
}

static inline int32_t clamp_s32_sample(int64_t value)
{
	if (value > INT32_MAX)
		return INT32_MAX;
	if (value < INT32_MIN)
		return INT32_MIN;
	return (int32_t)value;
}

static void ffmpeg_source_scale_audio_samples(enum audio_format format, uint8_t *dst, const uint8_t *src,
					      uint32_t frames, size_t channels, bool planar,
					      const float *gains)
{
	size_t samples = planar ? frames : (size_t)frames * channels;

	switch (format) {
	case AUDIO_FORMAT_U8BIT:
	case AUDIO_FORMAT_U8BIT_PLANAR:
		for (size_t i = 0; i < samples; i++) {
			float gain = gains[planar ? i : (i / channels)];
			int sample = (int)src[i] - 128;
			dst[i] = clamp_u8_sample((int)lrintf(sample * gain) + 128);
		}
		break;

	case AUDIO_FORMAT_16BIT:
	case AUDIO_FORMAT_16BIT_PLANAR: {
		int16_t *dst16 = (int16_t *)dst;
		const int16_t *src16 = (const int16_t *)src;

		for (size_t i = 0; i < samples; i++) {
			float gain = gains[planar ? i : (i / channels)];
			dst16[i] = clamp_s16_sample((int)lrintf((float)src16[i] * gain));
		}
		break;
	}

	case AUDIO_FORMAT_32BIT:
	case AUDIO_FORMAT_32BIT_PLANAR: {
		int32_t *dst32 = (int32_t *)dst;
		const int32_t *src32 = (const int32_t *)src;

		for (size_t i = 0; i < samples; i++) {
			float gain = gains[planar ? i : (i / channels)];
			dst32[i] = clamp_s32_sample((int64_t)llrint((double)src32[i] * gain));
		}
		break;
	}

	case AUDIO_FORMAT_FLOAT:
	case AUDIO_FORMAT_FLOAT_PLANAR: {
		float *dstf = (float *)dst;
		const float *srcf = (const float *)src;

		for (size_t i = 0; i < samples; i++) {
			float gain = gains[planar ? i : (i / channels)];
			dstf[i] = srcf[i] * gain;
		}
		break;
	}

	case AUDIO_FORMAT_UNKNOWN:
		break;
	}
}

static bool ffmpeg_source_get_output_audio_info(struct resample_info *info)
{
	audio_t *audio = obs_get_audio();
	const struct audio_output_info *output_info;

	if (!audio)
		return false;

	output_info = audio_output_get_info(audio);
	if (!output_info || !output_info->samples_per_sec || output_info->speakers == SPEAKERS_UNKNOWN)
		return false;

	info->samples_per_sec = output_info->samples_per_sec;
	info->speakers = output_info->speakers;
	info->format = AUDIO_FORMAT_FLOAT_PLANAR;
	return true;
}

static void ffmpeg_audio_packet_clear(struct ffmpeg_audio_packet *packet)
{
	packet->frames = 0;
	packet->timestamp = 0;
	packet->valid = false;
}

static void ffmpeg_audio_packet_free(struct ffmpeg_audio_packet *packet)
{
	for (size_t i = 0; i < MAX_AUDIO_CHANNELS; i++) {
		bfree(packet->data[i]);
		packet->data[i] = NULL;
	}

	packet->capacity_frames = 0;
	ffmpeg_audio_packet_clear(packet);
}

static bool ffmpeg_audio_packet_ensure_capacity(struct ffmpeg_audio_packet *packet, size_t channels, uint32_t frames)
{
	if (packet->capacity_frames >= frames)
		return true;

	ffmpeg_audio_packet_free(packet);
	for (size_t ch = 0; ch < channels; ch++)
		packet->data[ch] = bmalloc(sizeof(float) * frames);

	packet->capacity_frames = frames;
	return true;
}

static void ffmpeg_audio_stream_reset_resampler(struct ffmpeg_audio_stream_state *stream)
{
	audio_resampler_destroy(stream->resampler);
	stream->resampler = NULL;
	stream->input_info_valid = false;
	stream->resampler_failed = false;
}

static void ffmpeg_audio_stream_free(struct ffmpeg_audio_stream_state *stream)
{
	ffmpeg_audio_stream_reset_resampler(stream);
	ffmpeg_audio_packet_free(&stream->packet);
}

static bool ffmpeg_audio_stream_update_resampler(struct ffmpeg_audio_stream_state *stream,
						 const struct obs_source_audio *audio,
						 const struct resample_info *output_info)
{
	const bool matches_output = audio->samples_per_sec == output_info->samples_per_sec &&
					  audio->speakers == output_info->speakers &&
					  audio->format == output_info->format;
	const bool same_input = stream->input_info_valid && stream->input_info.samples_per_sec == audio->samples_per_sec &&
					 stream->input_info.speakers == audio->speakers &&
					 stream->input_info.format == audio->format;

	if (same_input)
		return !stream->resampler_failed;

	ffmpeg_audio_stream_reset_resampler(stream);
	stream->input_info.samples_per_sec = audio->samples_per_sec;
	stream->input_info.speakers = audio->speakers;
	stream->input_info.format = audio->format;
	stream->input_info_valid = true;

	if (matches_output)
		return true;

	stream->resampler = audio_resampler_create(output_info, &stream->input_info);
	stream->resampler_failed = stream->resampler == NULL;
	return !stream->resampler_failed;
}

static bool ffmpeg_audio_stream_store_packet(struct ffmpeg_audio_stream_state *stream, const struct obs_source_audio *audio)
{
	struct resample_info output_info;
	size_t channels;
	uint64_t ts_offset = 0;
	uint32_t out_frames = audio->frames;
	uint64_t timestamp = audio->timestamp;
	uint8_t *resampled[MAX_AV_PLANES] = {0};

	if (!ffmpeg_source_get_output_audio_info(&output_info) || !audio->frames)
		return false;
	if (!ffmpeg_audio_stream_update_resampler(stream, audio, &output_info))
		return false;

	channels = get_audio_channels(output_info.speakers);
	if (stream->resampler) {
		if (!audio_resampler_resample(stream->resampler, resampled, &out_frames, &ts_offset, audio->data,
					     audio->frames)) {
			return false;
		}
		timestamp = timestamp > ts_offset ? timestamp - ts_offset : 0;
	} else {
		for (size_t ch = 0; ch < channels; ch++)
			resampled[ch] = (uint8_t *)audio->data[ch];
	}

	ffmpeg_audio_packet_ensure_capacity(&stream->packet, channels, out_frames);
	for (size_t ch = 0; ch < channels; ch++)
		memcpy(stream->packet.data[ch], resampled[ch], sizeof(float) * out_frames);

	stream->packet.frames = out_frames;
	stream->packet.timestamp = timestamp;
	stream->packet.valid = true;
	return true;
}

static bool ffmpeg_source_ensure_mix_audio_capacity(struct ffmpeg_source *s, size_t channels, uint32_t frames)
{
	if (s->mix_audio_capacity_frames >= frames)
		return true;

	for (size_t ch = 0; ch < MAX_AUDIO_CHANNELS; ch++) {
		bfree(s->mix_audio_buffer[ch]);
		s->mix_audio_buffer[ch] = NULL;
	}

	for (size_t ch = 0; ch < channels; ch++)
		s->mix_audio_buffer[ch] = bmalloc(sizeof(float) * frames);

	s->mix_audio_capacity_frames = frames;
	return true;
}

static void ffmpeg_source_output_float_audio(struct ffmpeg_source *s, uint32_t frames, uint64_t timestamp)
{
	struct resample_info output_info;
	struct obs_source_audio audio = {0};
	size_t channels;

	if (!ffmpeg_source_get_output_audio_info(&output_info))
		return;

	channels = get_audio_channels(output_info.speakers);
	for (size_t ch = 0; ch < channels; ch++)
		audio.data[ch] = (const uint8_t *)s->mix_audio_buffer[ch];

	audio.frames = frames;
	audio.timestamp = timestamp;
	audio.samples_per_sec = output_info.samples_per_sec;
	audio.speakers = output_info.speakers;
	audio.format = output_info.format;
	obs_source_output_audio(s->source, &audio);
}

static float ffmpeg_source_get_crossfade_gain(const struct ffmpeg_source *s, bool transition, uint64_t timestamp)
{
	uint64_t duration_ns;
	uint64_t elapsed;
	float progress;

	if (!s->transition_active || !ffmpeg_source_is_crossfade_mode(s) || !s->transition_start_ns)
		return transition ? 0.0f : 1.0f;

	duration_ns = ffmpeg_source_transition_duration_ns(s);
	if (!duration_ns)
		return transition ? 0.0f : 1.0f;

	elapsed = timestamp > s->transition_start_ns ? timestamp - s->transition_start_ns : 0;
	if (elapsed >= duration_ns)
		return transition ? 0.0f : 1.0f;

	progress = (float)elapsed / (float)duration_ns;
	return transition ? 1.0f - progress : progress;
}

static void ffmpeg_source_mix_audio_packet(struct ffmpeg_source *s, const struct ffmpeg_audio_packet *packet,
					   bool transition, size_t channels,
					   uint32_t sample_rate, uint32_t offset)
{
	for (uint32_t frame = 0; frame < packet->frames; frame++) {
		uint64_t frame_ts = packet->timestamp + audio_frames_to_ns(sample_rate, frame);
		float gain = ffmpeg_source_get_crossfade_gain(s, transition, frame_ts);

		if (fabsf(gain) <= 0.0001f)
			continue;

		for (size_t ch = 0; ch < channels; ch++)
			s->mix_audio_buffer[ch][offset + frame] += packet->data[ch][frame] * gain;
	}
}

static void ffmpeg_source_output_crossfade_mix(struct ffmpeg_source *s, const struct ffmpeg_audio_packet *current_packet,
					       const struct ffmpeg_audio_packet *transition_packet)
{
	struct resample_info output_info;
	uint64_t start_ts;
	uint32_t total_frames = 0;
	uint32_t current_offset = 0;
	uint32_t transition_offset = 0;
	size_t channels;

	if (!current_packet && !transition_packet)
		return;
	if (!ffmpeg_source_get_output_audio_info(&output_info))
		return;

	channels = get_audio_channels(output_info.speakers);
	start_ts = current_packet ? current_packet->timestamp : transition_packet->timestamp;
	if (transition_packet && transition_packet->timestamp < start_ts)
		start_ts = transition_packet->timestamp;

	if (current_packet) {
		current_offset = current_packet->timestamp > start_ts
				 ? (uint32_t)ns_to_audio_frames(output_info.samples_per_sec,
						     current_packet->timestamp - start_ts)
				 : 0;
		total_frames = current_offset + current_packet->frames;
	}

	if (transition_packet) {
		transition_offset = transition_packet->timestamp > start_ts
				    ? (uint32_t)ns_to_audio_frames(output_info.samples_per_sec,
						        transition_packet->timestamp - start_ts)
				    : 0;
		if (transition_offset + transition_packet->frames > total_frames)
			total_frames = transition_offset + transition_packet->frames;
	}

	if (!total_frames)
		return;

	ffmpeg_source_ensure_mix_audio_capacity(s, channels, total_frames);
	for (size_t ch = 0; ch < channels; ch++)
		memset(s->mix_audio_buffer[ch], 0, sizeof(float) * total_frames);

	if (transition_packet)
		ffmpeg_source_mix_audio_packet(s, transition_packet, true, channels, output_info.samples_per_sec,
					       transition_offset);
	if (current_packet)
		ffmpeg_source_mix_audio_packet(s, current_packet, false, channels, output_info.samples_per_sec,
					       current_offset);

	ffmpeg_source_output_float_audio(s, total_frames, start_ts);
}

static void ffmpeg_source_try_output_crossfade_audio(struct ffmpeg_source *s)
{
	struct ffmpeg_audio_packet *current_packet = &s->current_callback->audio.packet;
	struct ffmpeg_audio_packet *transition_packet = s->transition_callback ? &s->transition_callback->audio.packet : NULL;
	const bool current_valid = current_packet->valid;
	const bool transition_valid = transition_packet && transition_packet->valid;
	const bool current_has_audio = s->media && media_playback_has_audio(s->media);
	const bool transition_has_audio = s->transition_media && media_playback_has_audio(s->transition_media);

	if (!current_valid && !transition_valid)
		return;

	if (!ffmpeg_source_is_crossfade_mode(s) || !s->transition_active) {
		if (current_valid) {
			ffmpeg_source_output_crossfade_mix(s, current_packet, NULL);
			ffmpeg_audio_packet_clear(current_packet);
		}
		if (transition_valid)
			ffmpeg_audio_packet_clear(transition_packet);
		return;
	}

	if (current_valid && transition_valid) {
		ffmpeg_source_output_crossfade_mix(s, current_packet, transition_packet);
		ffmpeg_audio_packet_clear(current_packet);
		ffmpeg_audio_packet_clear(transition_packet);
		return;
	}

	if (current_valid && !transition_has_audio) {
		ffmpeg_source_output_crossfade_mix(s, current_packet, NULL);
		ffmpeg_audio_packet_clear(current_packet);
		return;
	}

	if (transition_valid && !current_has_audio) {
		ffmpeg_source_output_crossfade_mix(s, NULL, transition_packet);
		ffmpeg_audio_packet_clear(transition_packet);
	}
}

static void ffmpeg_source_output_audio_with_transition(struct ffmpeg_media_callback *callback,
					       struct obs_source_audio *audio)
{
	struct ffmpeg_source *s = callback->source;
	float gains[AUDIO_OUTPUT_FRAMES];
	const bool planar = is_audio_planar(audio->format);
	const size_t channels = get_audio_channels(audio->speakers);
	const size_t planes = get_audio_planes(audio->format, audio->speakers);
	const size_t plane_size = get_audio_size(audio->format, audio->speakers, audio->frames);
	struct obs_source_audio scaled = *audio;
	const bool use_crossfade = ffmpeg_source_is_crossfade_mode(s) && (s->transition_active || callback->transition);

	if (use_crossfade) {
		ffmpeg_source_try_output_crossfade_audio(s);
		if (callback->audio.packet.valid) {
			ffmpeg_source_output_crossfade_mix(s, callback->transition ? NULL : &callback->audio.packet,
						  callback->transition ? &callback->audio.packet : NULL);
			ffmpeg_audio_packet_clear(&callback->audio.packet);
		}

		if (ffmpeg_audio_stream_store_packet(&callback->audio, audio))
			ffmpeg_source_try_output_crossfade_audio(s);
		return;
	}

	if (!audio->frames || audio->frames > AUDIO_OUTPUT_FRAMES || !channels || !planes || !plane_size) {
		obs_source_output_audio(s->source, audio);
		return;
	}

	if (!ffmpeg_source_get_audio_transition_gains(s, audio, gains)) {
		obs_source_output_audio(s->source, audio);
		return;
	}

	if (s->transition_audio_buffer_size < plane_size) {
		ffmpeg_source_free_transition_audio(s);
		for (size_t i = 0; i < MAX_AV_PLANES; i++)
			s->transition_audio_buffer[i] = bmalloc(plane_size);
		s->transition_audio_buffer_size = plane_size;
	}

	for (size_t plane = 0; plane < planes; plane++) {
		ffmpeg_source_scale_audio_samples(audio->format, s->transition_audio_buffer[plane], audio->data[plane],
						  audio->frames, channels, planar, gains);
		scaled.data[plane] = s->transition_audio_buffer[plane];
	}

	obs_source_output_audio(s->source, &scaled);
}

// Used to safely cancel and join any active reconnect threads
// Use this to join any finished reconnect thread too!
static void stop_reconnect_thread(struct ffmpeg_source *s)
{
	if (s->is_local_file)
		return;
	pthread_mutex_lock(&s->reconnect_mutex);
	if (s->reconnect_thread_valid) {
		os_event_signal(s->reconnect_stop_event);
		pthread_join(s->reconnect_thread, NULL);
		s->reconnect_thread_valid = false;
		os_atomic_set_bool(&s->reconnecting, false);
		os_event_reset(s->reconnect_stop_event);
	}
	pthread_mutex_unlock(&s->reconnect_mutex);
}

static void set_media_state(void *data, enum obs_media_state state)
{
	struct ffmpeg_source *s = data;
	s->state = state;
}

static void free_playlist(media_playlist_t *playlist)
{
	for (size_t i = 0; i < playlist->num; i++)
		bfree(playlist->array[i]);

	da_free(*playlist);
}

static bool path_is_url(const char *path)
{
	return path && strstr(path, "://") != NULL;
}

static void add_playlist_item(media_playlist_t *playlist, const char *path)
{
	struct dstr normalized_path = {0};
	char *copy;

	if (!path || !*path)
		return;

	dstr_copy(&normalized_path, path);
#ifdef _WIN32
	if (!path_is_url(path))
		dstr_replace(&normalized_path, "/", "\\");
#endif

	copy = bstrdup(normalized_path.array);
	da_push_back(*playlist, &copy);
	dstr_free(&normalized_path);
}

static bool string_equals(const char *lhs, const char *rhs)
{
	if (!lhs)
		lhs = "";
	if (!rhs)
		rhs = "";

	return strcmp(lhs, rhs) == 0;
}

static bool valid_extension(const char *ext)
{
	struct dstr test = {0};
	bool valid = false;
	const char *extensions = ".mp4;.m4v;.ts;.mov;.mxf;.flv;.mkv;.avi;.mp3;.ogg;.aac;.wav;.gif;.webm";
	const char *begin;
	const char *end;

	if (!ext || !*ext)
		return false;

	begin = extensions;
	end = strchr(begin, ';');

	for (;;) {
		if (end)
			dstr_ncopy(&test, begin, end - begin);
		else
			dstr_copy(&test, begin);

		if (dstr_cmpi(&test, ext) == 0) {
			valid = true;
			break;
		}

		if (!end)
			break;

		begin = end + 1;
		end = strchr(begin, ';');
	}

	dstr_free(&test);
	return valid;
}

static void load_playlist_entry(media_playlist_t *playlist, const char *path)
{
	os_dir_t *dir;
	struct dstr dir_path = {0};
	struct os_dirent *ent;

	if (!path || !*path)
		return;

	dir = path_is_url(path) ? NULL : os_opendir(path);
	if (!dir) {
		add_playlist_item(playlist, path);
		return;
	}

	for (;;) {
		const char *ext;

		ent = os_readdir(dir);
		if (!ent)
			break;
		if (ent->directory)
			continue;

		ext = os_get_path_extension(ent->d_name);
		if (!valid_extension(ext))
			continue;

		dstr_copy(&dir_path, path);
		dstr_cat_ch(&dir_path, '/');
		dstr_cat(&dir_path, ent->d_name);
		add_playlist_item(playlist, dir_path.array);
	}

	dstr_free(&dir_path);
	os_closedir(dir);
}

static void load_playlist(media_playlist_t *playlist, obs_data_t *settings)
{
	obs_data_array_t *array = obs_data_get_array(settings, S_PLAYLIST);
	size_t count = obs_data_array_count(array);

	da_init(*playlist);

	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(array, i);
		const char *value = obs_data_get_string(item, "value");

		load_playlist_entry(playlist, value);

		obs_data_release(item);
	}

	obs_data_array_release(array);
}

static bool playlist_equals(const media_playlist_t *lhs, const media_playlist_t *rhs)
{
	if (lhs->num != rhs->num)
		return false;

	for (size_t i = 0; i < lhs->num; i++) {
		if (strcmp(lhs->array[i], rhs->array[i]) != 0)
			return false;
	}

	return true;
}

static bool find_playlist_index(const media_playlist_t *playlist, const char *path, size_t *index)
{
	if (!path || !*path)
		return false;

	for (size_t i = 0; i < playlist->num; i++) {
		if (strcmp(playlist->array[i], path) == 0) {
			*index = i;
			return true;
		}
	}

	return false;
}

static bool ffmpeg_source_has_playlist(const struct ffmpeg_source *s)
{
	return ffmpeg_source_supports_playlist(s) && s->playlist.num > 0;
}

static void ffmpeg_source_signal_playlist_updated(struct ffmpeg_source *s)
{
	signal_handler_t *sh = obs_source_get_signal_handler(s->source);
	calldata_t cd = {0};

	calldata_set_int(&cd, "count", (int)s->playlist.num);
	signal_handler_signal(sh, "playlist_updated", &cd);
	calldata_free(&cd);
}

static void ffmpeg_source_signal_playlist_selection_changed(struct ffmpeg_source *s)
{
	signal_handler_t *sh = obs_source_get_signal_handler(s->source);
	calldata_t cd = {0};
	int index = ffmpeg_source_has_playlist(s) ? (int)s->playlist_index : -1;

	calldata_set_int(&cd, "index", index);
	calldata_set_string(&cd, "path", s->active_input ? s->active_input : "");
	signal_handler_signal(sh, "playlist_selection_changed", &cd);
	calldata_free(&cd);
}

static bool ffmpeg_source_resolve_active_input(struct ffmpeg_source *s)
{
	const char *active_input = s->input;
	bool active_is_local_file = s->configured_is_local_file;
	bool changed;

	if (ffmpeg_source_has_playlist(s)) {
		if (s->playlist_index >= s->playlist.num)
			s->playlist_index = 0;

		active_input = s->playlist.array[s->playlist_index];
		active_is_local_file = !path_is_url(active_input);
	}

	changed = !string_equals(s->active_input, active_input) || s->is_local_file != active_is_local_file;

	bfree(s->active_input);
	s->active_input = active_input ? bstrdup(active_input) : NULL;
	s->is_local_file = active_is_local_file;

	return changed;
}

static bool ffmpeg_source_get_playlist_step_index(const struct ffmpeg_source *s, int step, size_t *index)
{
	if (!ffmpeg_source_has_playlist(s))
		return false;

	if (step > 0) {
		if (s->playlist_index + 1 < s->playlist.num) {
			*index = s->playlist_index + 1;
			return true;
		}

		if (!s->is_looping)
			return false;

		*index = 0;
		return true;
	}

	if (s->playlist_index > 0) {
		*index = s->playlist_index - 1;
		return true;
	}

	if (!s->is_looping)
		return false;

	*index = s->playlist.num - 1;
	return true;
}

static bool is_local_file_modified_internal(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings,
					    bool playlist_source)
{
	UNUSED_PARAMETER(prop);

	bool enabled = obs_data_get_bool(settings, "is_local_file");
	obs_property_t *is_local = obs_properties_get(props, "is_local_file");
	obs_property_t *input = obs_properties_get(props, "input");
	obs_property_t *input_format = obs_properties_get(props, "input_format");
	obs_property_t *local_file = obs_properties_get(props, "local_file");
	obs_property_t *looping = obs_properties_get(props, "looping");
	obs_property_t *buffering = obs_properties_get(props, "buffering_mb");
	obs_property_t *seekable = obs_properties_get(props, "seekable");
	obs_property_t *speed = obs_properties_get(props, "speed_percent");
	obs_property_t *reconnect_delay_sec = obs_properties_get(props, "reconnect_delay_sec");
	obs_property_set_visible(is_local, !playlist_source);
	obs_property_set_visible(input, !enabled);
	obs_property_set_visible(input_format, !enabled);
	obs_property_set_visible(buffering, !enabled);
	obs_property_set_visible(local_file, enabled && !playlist_source);
	obs_property_set_visible(looping, playlist_source || enabled);
	obs_property_set_visible(speed, enabled);
	obs_property_set_visible(seekable, !enabled);
	obs_property_set_visible(reconnect_delay_sec, !enabled);

	return true;
}

static bool is_local_file_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	return is_local_file_modified_internal(props, prop, settings, true);
}

static bool media_source_is_local_file_modified(obs_properties_t *props, obs_property_t *prop,
						obs_data_t *settings)
{
	return is_local_file_modified_internal(props, prop, settings, false);
}

static void ffmpeg_source_defaults_internal(obs_data_t *settings, bool playlist_source)
{
	obs_data_set_default_bool(settings, "is_local_file", true);
	obs_data_set_default_bool(settings, "looping", false);
	obs_data_set_default_bool(settings, "clear_on_media_end", true);
	obs_data_set_default_bool(settings, "restart_on_activate", true);
	obs_data_set_default_bool(settings, "linear_alpha", false);
	obs_data_set_default_int(settings, "reconnect_delay_sec", 10);
	obs_data_set_default_int(settings, "buffering_mb", 2);
	obs_data_set_default_int(settings, "speed_percent", 100);
	obs_data_set_default_bool(settings, "log_changes", true);

	if (!playlist_source)
		return;

	obs_data_set_default_bool(settings, "auto_fit_to_screen", false);
	obs_data_set_default_bool(settings, "blur_fill", false);
	obs_data_set_default_int(settings, "blur_fill_strength", 35);
	obs_data_set_default_int(settings, "blur_fill_opacity", 100);
	obs_data_set_default_int(settings, "transition_mode", FFMPEG_TRANSITION_CROSSFADE);
	obs_data_set_default_int(settings, "transition_ms", 0);
}

static void ffmpeg_source_defaults(obs_data_t *settings)
{
	ffmpeg_source_defaults_internal(settings, true);
}

static void ffmpeg_media_source_defaults(obs_data_t *settings)
{
	ffmpeg_source_defaults_internal(settings, false);
}

static void ffmpeg_source_update_private_settings(struct ffmpeg_source *s, obs_data_t *settings,
						  bool bump_transition_serial)
{
	obs_data_t *private_settings = obs_source_get_private_settings(s->source);

	obs_data_set_bool(private_settings, "ffmpeg_blur_fill", obs_data_get_bool(settings, "blur_fill"));
	obs_data_set_int(private_settings, "ffmpeg_blur_fill_strength",
			 obs_data_get_int(settings, "blur_fill_strength"));
	obs_data_set_int(private_settings, "ffmpeg_blur_fill_opacity",
			 obs_data_get_int(settings, "blur_fill_opacity"));
	obs_data_set_int(private_settings, "ffmpeg_transition_mode", s->transition_mode);
	obs_data_set_int(private_settings, "ffmpeg_transition_ms", obs_data_get_int(settings, "transition_ms"));
	obs_data_set_int(private_settings, "ffmpeg_transition_start_ns", (int64_t)s->transition_start_ns);

	if (bump_transition_serial)
		obs_data_set_int(private_settings, "ffmpeg_transition_serial", (int64_t)++s->transition_serial);

	obs_data_release(private_settings);
}

static void ffmpeg_source_refresh_private_settings(struct ffmpeg_source *s, bool bump_transition_serial)
{
	obs_data_t *settings = obs_source_get_settings(s->source);
	ffmpeg_source_update_private_settings(s, settings, bump_transition_serial);
	obs_data_release(settings);
}

static bool ffmpeg_source_live_preview_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	struct ffmpeg_source *s = obs_properties_get_param(props);
	int transition_ms;

	UNUSED_PARAMETER(prop);

	if (!s || !ffmpeg_source_supports_playlist(s))
		return true;

	s->transition_mode = ffmpeg_source_clamp_transition_mode((int)obs_data_get_int(settings, "transition_mode"));
	transition_ms = (int)obs_data_get_int(settings, "transition_ms");
	if (transition_ms < 0)
		transition_ms = 0;
	s->transition_ms = transition_ms;
	ffmpeg_source_update_private_settings(s, settings, false);
	return false;
}

static const char *media_filter =
	" (*.mp4 *.m4v *.ts *.mov *.mxf *.flv *.mkv *.avi *.mp3 *.ogg *.aac *.wav *.gif *.webm);;";
static const char *video_filter = " (*.mp4 *.m4v *.ts *.mov *.mxf *.flv *.mkv *.avi *.gif *.webm);;";
static const char *audio_filter = " (*.mp3 *.aac *.ogg *.wav);;";

static obs_properties_t *ffmpeg_source_getproperties_internal(void *data, bool playlist_source)
{
	struct ffmpeg_source *s = data;
	struct dstr filter = {0};
	struct dstr path = {0};

	obs_properties_t *props = obs_properties_create();

	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);
	obs_properties_set_param(props, s, NULL);

	obs_property_t *prop;
	// use this when obs allows non-readonly paths
	prop = obs_properties_add_bool(props, "is_local_file", obs_module_text("LocalFile"));

	obs_property_set_modified_callback(prop,
					  playlist_source ? is_local_file_modified
							  : media_source_is_local_file_modified);

	dstr_copy(&filter, obs_module_text("MediaFileFilter.AllMediaFiles"));
	dstr_cat(&filter, media_filter);
	dstr_cat(&filter, obs_module_text("MediaFileFilter.VideoFiles"));
	dstr_cat(&filter, video_filter);
	dstr_cat(&filter, obs_module_text("MediaFileFilter.AudioFiles"));
	dstr_cat(&filter, audio_filter);
	dstr_cat(&filter, obs_module_text("MediaFileFilter.AllFiles"));
	dstr_cat(&filter, " (*.*)");

	if (s) {
		const char *slash;
		const char *browse_path = NULL;

		if (playlist_source) {
			for (size_t i = s->playlist.num; i > 0; i--) {
				const char *candidate = s->playlist.array[i - 1];
				if (candidate && *candidate && !path_is_url(candidate)) {
					browse_path = candidate;
					break;
				}
			}
		}

		if (!browse_path && s->input && *s->input && !path_is_url(s->input))
			browse_path = s->input;

		if (browse_path) {
			dstr_copy(&path, browse_path);
			dstr_replace(&path, "\\", "/");
			slash = strrchr(path.array, '/');
			if (slash)
				dstr_resize(&path, slash - path.array + 1);
		}
	}

	if (playlist_source) {
		obs_properties_add_editable_list(props, S_PLAYLIST, obs_module_text("Playlist"),
						 OBS_EDITABLE_LIST_TYPE_FILES_AND_URLS, filter.array,
						 path.array);
	}

	obs_properties_add_path(props, "local_file", obs_module_text("LocalFile"), OBS_PATH_FILE, filter.array,
				path.array);

	obs_properties_add_bool(props, "looping", obs_module_text("Looping"));

	obs_properties_add_bool(props, "restart_on_activate", obs_module_text("RestartWhenActivated"));

	prop = obs_properties_add_int_slider(props, "buffering_mb", obs_module_text("BufferingMB"), 0, 16, 1);
	obs_property_int_set_suffix(prop, " MB");

	obs_properties_add_text(props, "input", obs_module_text("Input"), OBS_TEXT_DEFAULT);

	obs_properties_add_text(props, "input_format", obs_module_text("InputFormat"), OBS_TEXT_DEFAULT);

	prop = obs_properties_add_int_slider(props, "reconnect_delay_sec", obs_module_text("ReconnectDelayTime"), 1, 60,
					     1);
	obs_property_int_set_suffix(prop, " S");

	obs_properties_add_bool(props, "hw_decode", obs_module_text("HardwareDecode"));

	obs_properties_add_bool(props, "clear_on_media_end", obs_module_text("ClearOnMediaEnd"));

	prop = obs_properties_add_bool(props, "close_when_inactive", obs_module_text("CloseFileWhenInactive"));

	obs_property_set_long_description(prop, obs_module_text("CloseFileWhenInactive.ToolTip"));

	prop = obs_properties_add_int_slider(props, "speed_percent", obs_module_text("SpeedPercentage"), 1, 200, 1);
	obs_property_int_set_suffix(prop, "%");

	prop = obs_properties_add_list(props, "color_range", obs_module_text("ColorRange"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("ColorRange.Auto"), VIDEO_RANGE_DEFAULT);
	obs_property_list_add_int(prop, obs_module_text("ColorRange.Partial"), VIDEO_RANGE_PARTIAL);
	obs_property_list_add_int(prop, obs_module_text("ColorRange.Full"), VIDEO_RANGE_FULL);

	obs_properties_add_bool(props, "linear_alpha", obs_module_text("LinearAlpha"));

	obs_properties_add_bool(props, "seekable", obs_module_text("Seekable"));

	prop = obs_properties_add_text(props, "ffmpeg_options", obs_module_text("FFmpegOpts"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(prop, obs_module_text("FFmpegOpts.ToolTip.Source"));

	if (playlist_source) {
		obs_properties_add_bool(props, "auto_fit_to_screen", obs_module_text("AutoFitToScreen"));
		prop = obs_properties_add_bool(props, "blur_fill", obs_module_text("BlurFillBackground"));
		obs_property_set_modified_callback(prop, ffmpeg_source_live_preview_modified);
		prop = obs_properties_add_int_slider(props, "blur_fill_strength", obs_module_text("BlurFillStrength"),
					     0, 100, 1);
		obs_property_set_modified_callback(prop, ffmpeg_source_live_preview_modified);
		obs_property_int_set_suffix(prop, "%");
		prop = obs_properties_add_int_slider(props, "blur_fill_opacity", obs_module_text("BlurFillOpacity"), 0,
					     100, 1);
		obs_property_set_modified_callback(prop, ffmpeg_source_live_preview_modified);
		obs_property_int_set_suffix(prop, "%");
		prop = obs_properties_add_list(props, "transition_mode", obs_module_text("TransitionMode"),
					 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(prop, obs_module_text("TransitionMode.Cut"), FFMPEG_TRANSITION_CUT);
		obs_property_list_add_int(prop, obs_module_text("TransitionMode.Fade"), FFMPEG_TRANSITION_FADE);
		obs_property_list_add_int(prop, obs_module_text("TransitionMode.Crossfade"),
					  FFMPEG_TRANSITION_CROSSFADE);
		obs_property_set_modified_callback(prop, ffmpeg_source_live_preview_modified);
		prop = obs_properties_add_int_slider(props, "transition_ms", obs_module_text("TransitionDuration"), 0,
					     5000, 50);
		obs_property_set_modified_callback(prop, ffmpeg_source_live_preview_modified);
		obs_property_int_set_suffix(prop, " ms");
	}

	dstr_free(&filter);
	dstr_free(&path);

	return props;
}

static obs_properties_t *ffmpeg_source_getproperties(void *data)
{
	return ffmpeg_source_getproperties_internal(data, true);
}

static obs_properties_t *ffmpeg_media_source_getproperties(void *data)
{
	return ffmpeg_source_getproperties_internal(data, false);
}

static void dump_source_info(struct ffmpeg_source *s, const char *input, const char *input_format)
{
	if (!s->log_changes)
		return;
	FF_BLOG(LOG_INFO,
		"settings:\n"
		"\tinput:                   %s\n"
		"\tinput_format:            %s\n"
		"\tspeed:                   %d\n"
		"\tis_looping:              %s\n"
		"\tis_linear_alpha:         %s\n"
		"\tis_hw_decoding:          %s\n"
		"\tis_clear_on_media_end:   %s\n"
		"\trestart_on_activate:     %s\n"
		"\tclose_when_inactive:     %s\n"
		"\tfull_decode:             %s\n"
		"\tffmpeg_options:          %s",
		input ? input : "(null)", input_format ? input_format : "(null)", s->speed_percent,
		s->is_looping ? "yes" : "no", s->is_linear_alpha ? "yes" : "no", s->is_hw_decoding ? "yes" : "no",
		s->is_clear_on_media_end ? "yes" : "no", s->restart_on_activate ? "yes" : "no",
		s->close_when_inactive ? "yes" : "no", s->full_decode ? "yes" : "no", s->ffmpeg_options);
}

static void get_frame(void *opaque, struct obs_source_frame *f)
{
	struct ffmpeg_media_callback *callback = opaque;
	struct ffmpeg_source *s = callback->source;

	if (callback->transition)
		return;

	obs_source_output_video(s->source, f);
}

static void preload_frame(void *opaque, struct obs_source_frame *f)
{
	struct ffmpeg_media_callback *callback = opaque;
	struct ffmpeg_source *s = callback->source;

	if (callback->transition)
		return;

	if (s->close_when_inactive)
		return;

	if (s->is_clear_on_media_end || s->is_looping)
		obs_source_preload_video(s->source, f);

	if (!s->is_local_file && os_atomic_set_bool(&s->reconnecting, false))
		FF_BLOG(LOG_INFO, "Reconnected.");
}

static void seek_frame(void *opaque, struct obs_source_frame *f)
{
	struct ffmpeg_media_callback *callback = opaque;
	struct ffmpeg_source *s = callback->source;

	if (callback->transition)
		return;

	obs_source_set_video_frame(s->source, f);
}

static void get_audio(void *opaque, struct obs_source_audio *a)
{
	struct ffmpeg_media_callback *callback = opaque;
	struct ffmpeg_source *s = callback->source;
	ffmpeg_source_output_audio_with_transition(callback, a);

	if (!s->is_local_file && os_atomic_set_bool(&s->reconnecting, false))
		FF_BLOG(LOG_INFO, "Reconnected.");
}

static void media_stopped(void *opaque)
{
	struct ffmpeg_media_callback *callback = opaque;
	struct ffmpeg_source *s = callback->source;
	size_t next_index;

	if (callback->transition) {
		if (!s->suppress_transition_stop_callback)
			s->destroy_transition_media = true;
		return;
	}

	if (s->suppress_stop_callback)
		return;

	if (s->state != OBS_MEDIA_STATE_STOPPED && ffmpeg_source_get_playlist_step_index(s, 1, &next_index)) {
		if (ffmpeg_source_is_crossfade_mode(s)) {
			ffmpeg_source_start_crossfade_transition(s, next_index);
			ffmpeg_source_refresh_private_settings(s, false);
			return;
		}

		if (ffmpeg_source_is_fade_mode(s)) {
			ffmpeg_source_start_fade_transition(s, next_index, false);
			ffmpeg_source_refresh_private_settings(s, false);
		}

		s->pending_playlist_index = next_index;
		s->playlist_switch_pending = true;
		s->destroy_media = true;
		return;
	}

	if (s->is_clear_on_media_end && !s->is_track_matte) {
		obs_source_output_video(s->source, NULL);
	}

	if ((s->close_when_inactive || !s->is_local_file) && s->media)
		s->destroy_media = true;

	if (s->state != OBS_MEDIA_STATE_STOPPED) {
		set_media_state(s, OBS_MEDIA_STATE_ENDED);
		obs_source_media_ended(s->source);
	}
}

static void ffmpeg_source_open(struct ffmpeg_source *s)
{
	if (s->active_input && *s->active_input) {
		struct mp_media_info info = {
			.opaque = s->current_callback,
			.v_cb = get_frame,
			.v_preload_cb = preload_frame,
			.v_seek_cb = seek_frame,
			.a_cb = get_audio,
			.stop_cb = media_stopped,
			.path = s->active_input,
			.format = s->is_local_file ? NULL : s->input_format,
			.buffering = s->buffering_mb * 1024 * 1024,
			.speed = s->speed_percent,
			.force_range = s->range,
			.is_linear_alpha = s->is_linear_alpha,
			.hardware_decoding = s->is_hw_decoding,
			.ffmpeg_options = s->ffmpeg_options,
			.is_local_file = s->is_local_file || s->seekable,
			.reconnecting = s->reconnecting,
			.request_preload = s->is_stinger,
			.full_decode = s->full_decode,
		};

		s->media = media_playback_create(&info);
	}
}

static void ffmpeg_source_start(struct ffmpeg_source *s)
{
	bool looping = s->is_looping && !ffmpeg_source_has_playlist(s);

	if (!s->media)
		ffmpeg_source_open(s);

	if (!s->media)
		return;

	media_playback_play(s->media, looping, s->reconnecting);
	if (s->is_local_file && media_playback_has_video(s->media) && (s->is_clear_on_media_end || looping))
		obs_source_show_preloaded_video(s->source);
	else
		obs_source_output_video(s->source, NULL);
	set_media_state(s, OBS_MEDIA_STATE_PLAYING);
	obs_source_media_started(s->source);
}

static void ffmpeg_source_destroy_media(struct ffmpeg_source *s, bool suppress_stop_callback)
{
	if (!s->media)
		return;

	if (suppress_stop_callback)
		s->suppress_stop_callback = true;

	media_playback_destroy(s->media);
	s->media = NULL;
	s->suppress_stop_callback = false;
	ffmpeg_audio_packet_clear(&s->current_callback->audio.packet);
}

static void ffmpeg_source_destroy_transition_media(struct ffmpeg_source *s, bool suppress_stop_callback)
{
	if (!s->transition_media)
		return;

	if (suppress_stop_callback)
		s->suppress_transition_stop_callback = true;

	media_playback_destroy(s->transition_media);
	s->transition_media = NULL;
	s->suppress_transition_stop_callback = false;

	if (s->transition_callback) {
		s->transition_callback->transition = false;
		ffmpeg_audio_packet_clear(&s->transition_callback->audio.packet);
		s->transition_callback = NULL;
	}
}

static void ffmpeg_source_start_playlist_index(struct ffmpeg_source *s, size_t index)
{
	bool active = obs_source_active(s->source);

	s->playlist_index = index;
	s->playlist_switch_pending = false;
	s->destroy_media = false;
	ffmpeg_source_resolve_active_input(s);
	ffmpeg_source_refresh_private_settings(s, false);
	ffmpeg_source_signal_playlist_selection_changed(s);

	if ((!s->close_when_inactive || active) && s->active_input && *s->active_input)
		ffmpeg_source_open(s);

	if ((!s->restart_on_activate || active) && s->active_input && *s->active_input)
		ffmpeg_source_start(s);
}

static void ffmpeg_source_start_crossfade_transition(struct ffmpeg_source *s, size_t index)
{
	struct ffmpeg_media_callback *outgoing_callback;

	if (!s->media) {
		ffmpeg_source_start_playlist_index(s, index);
		return;
	}

	ffmpeg_source_destroy_transition_media(s, true);
	outgoing_callback = s->current_callback;
	outgoing_callback->transition = true;
	s->transition_callback = outgoing_callback;
	s->transition_media = s->media;
	s->media = NULL;

	s->current_callback = ffmpeg_source_other_callback(s, outgoing_callback);
	s->current_callback->transition = false;
	ffmpeg_audio_packet_clear(&s->current_callback->audio.packet);

	s->pending_playlist_index = index;
	s->transition_active = true;
	s->transition_midpoint_pending = false;
	s->transition_start_ns = os_gettime_ns();
	ffmpeg_source_refresh_private_settings(s, true);
	ffmpeg_source_start_playlist_index(s, index);
}

static void ffmpeg_source_switch_to_playlist_index(struct ffmpeg_source *s, size_t index)
{
	if (ffmpeg_source_is_crossfade_mode(s) && s->media && s->state == OBS_MEDIA_STATE_PLAYING) {
		ffmpeg_source_start_crossfade_transition(s, index);
		ffmpeg_source_refresh_private_settings(s, false);
		return;
	}

	if (ffmpeg_source_should_delay_switch(s)) {
		ffmpeg_source_start_fade_transition(s, index, true);
		ffmpeg_source_refresh_private_settings(s, false);
		return;
	}

	if (ffmpeg_source_is_fade_mode(s)) {
		ffmpeg_source_start_fade_transition(s, index, false);
		ffmpeg_source_refresh_private_settings(s, false);
	}

	stop_reconnect_thread(s);
	ffmpeg_source_destroy_media(s, true);
	ffmpeg_source_start_playlist_index(s, index);
}

static void *ffmpeg_source_reconnect(void *data)
{
	struct ffmpeg_source *s = data;

	int ret = os_event_timedwait(s->reconnect_stop_event, s->reconnect_delay_sec * 1000);
	if (ret == 0 || s->media)
		return NULL;

	bool active = obs_source_active(s->source);
	if (!s->close_when_inactive || active)
		ffmpeg_source_open(s);

	if (!s->restart_on_activate || active)
		ffmpeg_source_start(s);

	return NULL;
}

static void ffmpeg_source_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);

	struct ffmpeg_source *s = data;
	if (s->transition_active && ffmpeg_source_is_fade_mode(s)) {
		uint64_t now = os_gettime_ns();
		uint64_t duration_ns = ffmpeg_source_transition_duration_ns(s);
		uint64_t elapsed = now > s->transition_start_ns ? now - s->transition_start_ns : 0;

		if (s->transition_midpoint_pending && elapsed >= duration_ns / 2) {
			s->transition_midpoint_pending = false;
			stop_reconnect_thread(s);
			ffmpeg_source_destroy_media(s, true);
			ffmpeg_source_start_playlist_index(s, s->pending_playlist_index);
		}

		if (elapsed >= duration_ns) {
			ffmpeg_source_reset_transition(s);
			ffmpeg_source_refresh_private_settings(s, false);
		}
	} else if (s->transition_active && ffmpeg_source_is_crossfade_mode(s)) {
		uint64_t now = os_gettime_ns();
		uint64_t duration_ns = ffmpeg_source_transition_duration_ns(s);
		uint64_t elapsed = now > s->transition_start_ns ? now - s->transition_start_ns : 0;

		if (elapsed >= duration_ns) {
			ffmpeg_source_destroy_transition_media(s, true);
			ffmpeg_source_reset_transition(s);
			ffmpeg_source_refresh_private_settings(s, false);
		}
	}

	if (s->destroy_media) {
		bool playlist_switch_pending = s->playlist_switch_pending;
		size_t pending_playlist_index = s->pending_playlist_index;

		ffmpeg_source_destroy_media(s, true);

		s->destroy_media = false;

		if (playlist_switch_pending) {
			ffmpeg_source_start_playlist_index(s, pending_playlist_index);
			return;
		}

		if (!s->is_local_file) {
			pthread_mutex_lock(&s->reconnect_mutex);
			if (!os_atomic_set_bool(&s->reconnecting, true))
				FF_BLOG(LOG_WARNING, "Disconnected. "
						     "Reconnecting...");
			if (s->reconnect_thread_valid) {
				os_event_signal(s->reconnect_stop_event);
				pthread_join(s->reconnect_thread, NULL);
				s->reconnect_thread_valid = false;
				os_event_reset(s->reconnect_stop_event);
			}
			if (pthread_create(&s->reconnect_thread, NULL, ffmpeg_source_reconnect, s) != 0) {
				FF_BLOG(LOG_WARNING, "Could not create "
						     "reconnect thread");
				pthread_mutex_unlock(&s->reconnect_mutex);
				return;
			}
			s->reconnect_thread_valid = true;
			pthread_mutex_unlock(&s->reconnect_mutex);
		}
	}

	if (s->destroy_transition_media) {
		ffmpeg_source_destroy_transition_media(s, true);
		s->destroy_transition_media = false;
	}
}

#define RIST_PROTO "rist"

static void ffmpeg_source_update(void *data, obs_data_t *settings)
{
	struct ffmpeg_source *s = data;
	const bool supports_playlist = ffmpeg_source_supports_playlist(s);

	bool active = obs_source_active(s->source);
	bool configured_is_local_file = obs_data_get_bool(settings, "is_local_file");
	bool is_stinger = obs_data_get_bool(settings, "is_stinger");
	bool is_track_matte = obs_data_get_bool(settings, "is_track_matte");
	bool should_restart_media = (is_stinger != s->is_stinger);

	const char *input;
	const char *input_format;
	const char *ffmpeg_options;
	media_playlist_t new_playlist;
	bool playlist_changed;
	bool active_input_changed;
	char *old_active_input;

	bool is_hw_decoding;
	enum video_range_type range;
	bool is_linear_alpha;
	int speed_percent;
	bool is_looping;
	const char *restart_input;
	enum ffmpeg_transition_mode transition_mode;
	int transition_ms;

	if (supports_playlist)
		load_playlist(&new_playlist, settings);
	else
		da_init(new_playlist);
	playlist_changed = !playlist_equals(&s->playlist, &new_playlist);
	old_active_input = s->active_input ? bstrdup(s->active_input) : NULL;

	if (!supports_playlist || !new_playlist.num)
		should_restart_media |= configured_is_local_file != s->configured_is_local_file;

	bfree(s->input_format);
	is_looping = (supports_playlist || configured_is_local_file) ? obs_data_get_bool(settings, "looping") : false;

	if (configured_is_local_file) {
		input = obs_data_get_string(settings, "local_file");
		input_format = NULL;

		if ((!supports_playlist || !new_playlist.num) && s->input && !should_restart_media)
			should_restart_media |= strcmp(s->input, input) != 0;
	} else {
		if (!supports_playlist || !new_playlist.num)
			should_restart_media = true;
		input = obs_data_get_string(settings, "input");
		input_format = obs_data_get_string(settings, "input_format");
		s->reconnect_delay_sec = (int)obs_data_get_int(settings, "reconnect_delay_sec");
		s->reconnect_delay_sec = s->reconnect_delay_sec == 0 ? 10 : s->reconnect_delay_sec;
	}

	stop_reconnect_thread(s);

	is_hw_decoding = obs_data_get_bool(settings, "hw_decode");
	range = obs_data_get_int(settings, "color_range");
	speed_percent = (int)obs_data_get_int(settings, "speed_percent");
	if (speed_percent < 1 || speed_percent > 200)
		speed_percent = 100;
	ffmpeg_options = obs_data_get_string(settings, "ffmpeg_options");

	/* Restart media source if these properties are changed */
	if (s->is_hw_decoding != is_hw_decoding || s->range != range || s->speed_percent != speed_percent ||
	    !string_equals(s->input_format, input_format) || !string_equals(s->ffmpeg_options, ffmpeg_options))
		should_restart_media = true;

	/* If media has ended and user enables looping, user expects that it restarts.
	 * Should still check if is_looping was changed, because users may stop them
	 * intentionally, which is why we only check for ENDED and not STOPPED. */
	if (active && s->state == OBS_MEDIA_STATE_ENDED && is_looping == true && s->is_looping == false) {
		should_restart_media = true;
	}

	bfree(s->input);
	bfree(s->ffmpeg_options);

	s->is_looping = is_looping;
	s->configured_is_local_file = configured_is_local_file;
	s->close_when_inactive = obs_data_get_bool(settings, "close_when_inactive");
	s->input = input ? bstrdup(input) : NULL;
	s->input_format = input_format ? bstrdup(input_format) : NULL;
	s->is_hw_decoding = is_hw_decoding;
	s->full_decode = obs_data_get_bool(settings, "full_decode");
	s->is_clear_on_media_end = obs_data_get_bool(settings, "clear_on_media_end");
	s->restart_on_activate = !astrcmpi_n(input, RIST_PROTO, sizeof(RIST_PROTO) - 1)
					 ? false
					 : obs_data_get_bool(settings, "restart_on_activate");
	s->range = range;
	is_linear_alpha = obs_data_get_bool(settings, "linear_alpha");
	s->is_linear_alpha = is_linear_alpha;
	s->buffering_mb = (int)obs_data_get_int(settings, "buffering_mb");
	s->speed_percent = speed_percent;
	s->seekable = obs_data_get_bool(settings, "seekable");
	s->ffmpeg_options = ffmpeg_options ? bstrdup(ffmpeg_options) : NULL;
	s->is_stinger = is_stinger;
	s->is_track_matte = is_track_matte;
	s->log_changes = obs_data_get_bool(settings, "log_changes");
	transition_mode = ffmpeg_source_clamp_transition_mode((int)obs_data_get_int(settings, "transition_mode"));
	transition_ms = (int)obs_data_get_int(settings, "transition_ms");
	if (transition_ms < 0)
		transition_ms = 0;
	s->transition_mode = transition_mode;
	s->transition_ms = transition_ms;
	if (!ffmpeg_source_is_fade_mode(s) && !ffmpeg_source_is_crossfade_mode(s))
		ffmpeg_source_reset_transition(s);
	if (!ffmpeg_source_is_crossfade_mode(s) && s->transition_media) {
		ffmpeg_source_destroy_transition_media(s, true);
		ffmpeg_source_reset_transition(s);
	}

	if (playlist_changed) {
		free_playlist(&s->playlist);
		s->playlist = new_playlist;

		if (s->playlist.num) {
			size_t playlist_index = 0;

			if (find_playlist_index(&s->playlist, old_active_input, &playlist_index))
				s->playlist_index = playlist_index;
			else if (s->playlist_index >= s->playlist.num)
				s->playlist_index = 0;
		} else {
			s->playlist_index = 0;
		}
	} else {
		free_playlist(&new_playlist);
	}

	active_input_changed = ffmpeg_source_resolve_active_input(s);
	should_restart_media |= active_input_changed;
	restart_input = s->active_input ? s->active_input : "";
	bfree(old_active_input);
	ffmpeg_source_update_private_settings(s, settings, active_input_changed);

	if (playlist_changed)
		ffmpeg_source_signal_playlist_updated(s);
	if (playlist_changed || active_input_changed)
		ffmpeg_source_signal_playlist_selection_changed(s);

	if (s->speed_percent < 1 || s->speed_percent > 200)
		s->speed_percent = 100;

	s->restart_on_activate = !astrcmpi_n(restart_input, RIST_PROTO, sizeof(RIST_PROTO) - 1)
				     ? false
				     : obs_data_get_bool(settings, "restart_on_activate");

	if (s->media && should_restart_media) {
		ffmpeg_source_reset_transition(s);
		ffmpeg_source_destroy_media(s, true);
	}
	if (s->transition_media && should_restart_media)
		ffmpeg_source_destroy_transition_media(s, true);

	/* directly set options if media is playing */
	if (s->media) {
		media_playback_set_looping(s->media, s->is_looping && !ffmpeg_source_has_playlist(s));
		media_playback_set_is_linear_alpha(s->media, is_linear_alpha);
	}
	if ((!s->close_when_inactive || active) && should_restart_media)
		ffmpeg_source_open(s);

	dump_source_info(s, s->active_input, s->input_format);
	if ((!s->restart_on_activate || active) && should_restart_media)
		ffmpeg_source_start(s);
}

static const char *ffmpeg_source_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("FFmpegPlaylistSource");
}

static const char *ffmpeg_media_source_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("FFmpegSource");
}

static void restart_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return;

	struct ffmpeg_source *s = data;
	if (obs_source_showing(s->source))
		obs_source_media_restart(s->source);
}

static void restart_proc(void *data, calldata_t *cd)
{
	restart_hotkey(data, 0, NULL, true);
	UNUSED_PARAMETER(cd);
}

static void preload_first_frame_proc(void *data, calldata_t *cd)
{
	struct ffmpeg_source *s = data;
	if (!s->media)
		return;

	if (s->is_track_matte)
		obs_source_output_video(s->source, NULL);
	media_playback_preload_frame(s->media);
	UNUSED_PARAMETER(cd);
}

static void get_duration(void *data, calldata_t *cd)
{
	struct ffmpeg_source *s = data;
	int64_t dur = 0;
	if (s->media)
		dur = media_playback_get_duration(s->media);

	calldata_set_int(cd, "duration", dur * 1000);
}

static void get_nb_frames(void *data, calldata_t *cd)
{
	struct ffmpeg_source *s = data;
	int64_t frames = s->media ? media_playback_get_frames(s->media) : 0;
	calldata_set_int(cd, "num_frames", frames);
}

static void get_playlist_count(void *data, calldata_t *cd)
{
	struct ffmpeg_source *s = data;
	calldata_set_int(cd, "count", (int)s->playlist.num);
}

static void get_playlist_index(void *data, calldata_t *cd)
{
	struct ffmpeg_source *s = data;
	int index = ffmpeg_source_has_playlist(s) ? (int)s->playlist_index : -1;
	calldata_set_int(cd, "index", index);
}

static void get_playlist_item(void *data, calldata_t *cd)
{
	struct ffmpeg_source *s = data;
	int64_t index = calldata_int(cd, "index");
	const char *path = "";

	if (index >= 0 && (size_t)index < s->playlist.num)
		path = s->playlist.array[index];

	calldata_set_string(cd, "path", path);
}

static void set_playlist_index_proc(void *data, calldata_t *cd)
{
	struct ffmpeg_source *s = data;
	int64_t index = calldata_int(cd, "index");

	if (index < 0 || (size_t)index >= s->playlist.num)
		return;

	ffmpeg_source_switch_to_playlist_index(s, (size_t)index);
}

static bool ffmpeg_source_play_hotkey(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return false;

	struct ffmpeg_source *s = data;

	if (s->state == OBS_MEDIA_STATE_PLAYING || !obs_source_showing(s->source))
		return false;

	obs_source_media_play_pause(s->source, false);
	return true;
}

static bool ffmpeg_source_pause_hotkey(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return false;

	struct ffmpeg_source *s = data;

	if (s->state != OBS_MEDIA_STATE_PLAYING || !obs_source_showing(s->source))
		return false;

	obs_source_media_play_pause(s->source, true);
	return true;
}

static void ffmpeg_source_stop_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return;

	struct ffmpeg_source *s = data;

	if (obs_source_showing(s->source))
		obs_source_media_stop(s->source);
}

static void *ffmpeg_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct ffmpeg_source *s = bzalloc(sizeof(struct ffmpeg_source));
	const char *id = obs_source_get_unversioned_id(source);
	s->source = source;
	s->callback_a.source = s;
	s->callback_b.source = s;
	s->current_callback = &s->callback_a;
	s->playlist_source = ffmpeg_source_is_playlist_source_id(id);
	da_init(s->playlist);

	// Manual type since the event can be signalled without an active thread
	if (os_event_init(&s->reconnect_stop_event, OS_EVENT_TYPE_MANUAL)) {
		FF_BLOG(LOG_ERROR, "Failed to initialize reconnect stop event");
		bfree(s);
		return NULL;
	}

	if (pthread_mutex_init(&s->reconnect_mutex, NULL)) {
		FF_BLOG(LOG_ERROR, "Failed to initialize reconnect mutex");
		os_event_destroy(s->reconnect_stop_event);
		bfree(s);
		return NULL;
	}

	s->hotkey = obs_hotkey_register_source(source, "MediaSource.Restart", obs_module_text("RestartMedia"),
					       restart_hotkey, s);

	s->play_pause_hotkey = obs_hotkey_pair_register_source(s->source, "MediaSource.Play", obs_module_text("Play"),
							       "MediaSource.Pause", obs_module_text("Pause"),
							       ffmpeg_source_play_hotkey, ffmpeg_source_pause_hotkey, s,
							       s);

	s->stop_hotkey = obs_hotkey_register_source(source, "MediaSource.Stop", obs_module_text("Stop"),
						    ffmpeg_source_stop_hotkey, s);

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void restart()", restart_proc, s);
	proc_handler_add(ph, "void preload_first_frame()", preload_first_frame_proc, s);
	proc_handler_add(ph, "void get_duration(out int duration)", get_duration, s);
	proc_handler_add(ph, "void get_nb_frames(out int num_frames)", get_nb_frames, s);
	proc_handler_add(ph, "void get_playlist_count(out int count)", get_playlist_count, s);
	proc_handler_add(ph, "void get_playlist_index(out int index)", get_playlist_index, s);
	proc_handler_add(ph, "void get_playlist_item(in int index, out string path)", get_playlist_item, s);
	proc_handler_add(ph, "void set_playlist_index(in int index)", set_playlist_index_proc, s);

	signal_handler_t *sh = obs_source_get_signal_handler(source);
	signal_handler_add(sh, "void playlist_updated(int count)");
	signal_handler_add(sh, "void playlist_selection_changed(int index, string path)");

	ffmpeg_source_update(s, settings);
	return s;
}

static void ffmpeg_source_destroy(void *data)
{
	struct ffmpeg_source *s = data;

	stop_reconnect_thread(s);

	if (s->hotkey)
		obs_hotkey_unregister(s->hotkey);
	ffmpeg_source_destroy_media(s, true);
	ffmpeg_source_destroy_transition_media(s, true);
	ffmpeg_source_free_transition_audio(s);
	ffmpeg_audio_stream_free(&s->callback_a.audio);
	ffmpeg_audio_stream_free(&s->callback_b.audio);
	for (size_t i = 0; i < MAX_AUDIO_CHANNELS; i++)
		bfree(s->mix_audio_buffer[i]);

	pthread_mutex_destroy(&s->reconnect_mutex);
	os_event_destroy(s->reconnect_stop_event);
	free_playlist(&s->playlist);
	bfree(s->active_input);
	bfree(s->input);
	bfree(s->input_format);
	bfree(s->ffmpeg_options);
	bfree(s);
}

static void ffmpeg_source_activate(void *data)
{
	struct ffmpeg_source *s = data;

	if (s->restart_on_activate)
		obs_source_media_restart(s->source);
}

static void ffmpeg_source_deactivate(void *data)
{
	struct ffmpeg_source *s = data;

	if (s->restart_on_activate) {
		if (s->media) {
			media_playback_stop(s->media);
			if (s->transition_media)
				media_playback_stop(s->transition_media);

			if (s->is_clear_on_media_end)
				obs_source_output_video(s->source, NULL);
		}
	}
}

static void ffmpeg_source_play_pause(void *data, bool pause)
{
	struct ffmpeg_source *s = data;

	if (!s->media)
		ffmpeg_source_open(s);

	if (!s->media)
		return;

	media_playback_play_pause(s->media, pause);
	if (s->transition_media)
		media_playback_play_pause(s->transition_media, pause);

	if (pause) {

		set_media_state(s, OBS_MEDIA_STATE_PAUSED);
	} else {

		set_media_state(s, OBS_MEDIA_STATE_PLAYING);
		obs_source_media_started(s->source);
	}
}

static void ffmpeg_source_stop(void *data)
{
	struct ffmpeg_source *s = data;
	s->playlist_switch_pending = false;
	ffmpeg_source_reset_transition(s);

	if (s->media) {
		media_playback_stop(s->media);
		obs_source_output_video(s->source, NULL);
		set_media_state(s, OBS_MEDIA_STATE_STOPPED);
	}

	if (s->transition_media)
		ffmpeg_source_destroy_transition_media(s, true);
}

static void ffmpeg_source_restart(void *data)
{
	struct ffmpeg_source *s = data;
	ffmpeg_source_reset_transition(s);
	ffmpeg_source_destroy_transition_media(s, true);

	if (obs_source_showing(s->source))
		ffmpeg_source_start(s);

	set_media_state(s, OBS_MEDIA_STATE_PLAYING);
}

static void ffmpeg_source_playlist_next(void *data)
{
	struct ffmpeg_source *s = data;
	size_t playlist_index;

	if (!ffmpeg_source_get_playlist_step_index(s, 1, &playlist_index))
		return;

	ffmpeg_source_switch_to_playlist_index(s, playlist_index);
}

static void ffmpeg_source_playlist_previous(void *data)
{
	struct ffmpeg_source *s = data;
	size_t playlist_index;

	if (!ffmpeg_source_get_playlist_step_index(s, -1, &playlist_index))
		return;

	ffmpeg_source_switch_to_playlist_index(s, playlist_index);
}

static int64_t ffmpeg_source_get_duration(void *data)
{
	struct ffmpeg_source *s = data;
	int64_t dur = 0;

	if (s->media)
		dur = media_playback_get_duration(s->media) / INT64_C(1000);

	return dur;
}

static int64_t ffmpeg_source_get_time(void *data)
{
	struct ffmpeg_source *s = data;
	if (!s->media)
		return 0;

	return media_playback_get_current_time(s->media);
}

static void ffmpeg_source_set_time(void *data, int64_t ms)
{
	struct ffmpeg_source *s = data;

	if (!s->media)
		return;

	media_playback_seek(s->media, ms);
}

static enum obs_media_state ffmpeg_source_get_state(void *data)
{
	struct ffmpeg_source *s = data;

	return s->state;
}

static void missing_file_callback(void *src, const char *new_path, void *data)
{
	struct ffmpeg_source *s = src;

	obs_source_t *source = s->source;
	obs_data_t *settings = obs_source_get_settings(source);
	obs_data_set_string(settings, "local_file", new_path);
	obs_source_update(source, settings);
	obs_data_release(settings);

	UNUSED_PARAMETER(data);
}

static obs_missing_files_t *ffmpeg_source_missingfiles(void *data)
{
	struct ffmpeg_source *s = data;
	obs_missing_files_t *files = obs_missing_files_create();

	if (s->is_local_file && strcmp(s->input, "") != 0) {
		if (!os_file_exists(s->input)) {
			obs_missing_file_t *file = obs_missing_file_create(s->input, missing_file_callback,
									   OBS_MISSING_FILE_SOURCE, s->source, NULL);

			obs_missing_files_add_file(files, file);
		}
	}

	return files;
}

struct obs_source_info ffmpeg_media_source = {
	.id = FFMPEG_MEDIA_SOURCE_ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE |
			OBS_SOURCE_CONTROLLABLE_MEDIA,
	.get_name = ffmpeg_media_source_getname,
	.create = ffmpeg_source_create,
	.destroy = ffmpeg_source_destroy,
	.get_defaults = ffmpeg_media_source_defaults,
	.get_properties = ffmpeg_media_source_getproperties,
	.activate = ffmpeg_source_activate,
	.deactivate = ffmpeg_source_deactivate,
	.video_tick = ffmpeg_source_tick,
	.missing_files = ffmpeg_source_missingfiles,
	.update = ffmpeg_source_update,
	.icon_type = OBS_ICON_TYPE_MEDIA,
	.media_play_pause = ffmpeg_source_play_pause,
	.media_restart = ffmpeg_source_restart,
	.media_stop = ffmpeg_source_stop,
	.media_next = ffmpeg_source_playlist_next,
	.media_previous = ffmpeg_source_playlist_previous,
	.media_get_duration = ffmpeg_source_get_duration,
	.media_get_time = ffmpeg_source_get_time,
	.media_set_time = ffmpeg_source_set_time,
	.media_get_state = ffmpeg_source_get_state,
};

struct obs_source_info ffmpeg_source = {
	.id = FFMPEG_PLAYLIST_SOURCE_ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE |
			OBS_SOURCE_CONTROLLABLE_MEDIA,
	.get_name = ffmpeg_source_getname,
	.create = ffmpeg_source_create,
	.destroy = ffmpeg_source_destroy,
	.get_defaults = ffmpeg_source_defaults,
	.get_properties = ffmpeg_source_getproperties,
	.activate = ffmpeg_source_activate,
	.deactivate = ffmpeg_source_deactivate,
	.video_tick = ffmpeg_source_tick,
	.missing_files = ffmpeg_source_missingfiles,
	.update = ffmpeg_source_update,
	.icon_type = OBS_ICON_TYPE_MEDIA,
	.media_play_pause = ffmpeg_source_play_pause,
	.media_restart = ffmpeg_source_restart,
	.media_stop = ffmpeg_source_stop,
	.media_next = ffmpeg_source_playlist_next,
	.media_previous = ffmpeg_source_playlist_previous,
	.media_get_duration = ffmpeg_source_get_duration,
	.media_get_time = ffmpeg_source_get_time,
	.media_set_time = ffmpeg_source_set_time,
	.media_get_state = ffmpeg_source_get_state,
};
