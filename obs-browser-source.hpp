/******************************************************************************
 Copyright (C) 2014 by John R. Bradley <jrb@turrettech.com>
 Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 ******************************************************************************/

#pragma once

#include <obs-module.h>

#include "cef-headers.hpp"
#include "browser-app.hpp"
#include <atomic>
#include <functional>
#include <string>
#include <mutex>
#include <vector>

enum class ControlLevel : int {
	None,
	ReadObs,
	ReadUser,
	Basic,
	Advanced,
	All,
};
inline constexpr ControlLevel DEFAULT_CONTROL_LEVEL = ControlLevel::ReadObs;

enum BrowserTransitionMode {
	BROWSER_TRANSITION_CUT,
	BROWSER_TRANSITION_FADE,
	BROWSER_TRANSITION_CROSSFADE,
};

extern bool hwaccel;

struct BrowserSource {
	BrowserSource **p_prev_next = nullptr;
	BrowserSource *next = nullptr;

	obs_source_t *source = nullptr;

	bool tex_sharing_avail = false;
	bool create_browser = false;
	std::recursive_mutex lockBrowser;
	CefRefPtr<CefBrowser> cefBrowser;

	std::string url;
	std::string css;
	std::vector<std::string> playlist;
	gs_texture_t *texture = nullptr;
	gs_texture_t *extra_texture = nullptr;
	uint32_t last_cx = 0;
	uint32_t last_cy = 0;
	gs_color_format last_format = GS_UNKNOWN;

#ifdef ENABLE_BROWSER_SHARED_TEXTURE
#ifdef _WIN32
	void *last_handle = INVALID_HANDLE_VALUE;
#elif defined(__APPLE__)
	void *last_handle = nullptr;
#endif
#endif

	int width = 0;
	int height = 0;
	bool fps_custom = false;
	int fps = 0;
	double canvas_fps = 0;
	bool restart = false;
	bool shutdown_on_invisible = false;
	bool is_local = false;
	bool playlist_source = false;
	bool playlist_looping = false;
	bool rewrite_youtube = false;
	size_t playlist_index = 0;
	size_t pending_playlist_index = 0;
	int transition_ms = 0;
	BrowserTransitionMode transition_mode = BROWSER_TRANSITION_CROSSFADE;
	bool transition_active = false;
	bool transition_midpoint_pending = false;
	uint64_t transition_start_ns = 0;
	gs_texture_t *transition_texture = nullptr;
	bool first_update = true;
	bool reroute_audio = true;
	bool allow_mic = false;
	std::string mic_device;
	enum obs_media_state media_state = OBS_MEDIA_STATE_PLAYING;
	std::atomic<bool> destroying = false;
	ControlLevel webpage_control_level = DEFAULT_CONTROL_LEVEL;
#if defined(BROWSER_EXTERNAL_BEGIN_FRAME_ENABLED) && defined(ENABLE_BROWSER_SHARED_TEXTURE)
	bool reset_frame = false;
#endif
	bool is_showing = false;

	inline void DestroyTextures()
	{
		obs_enter_graphics();
		if (extra_texture) {
			gs_texture_destroy(extra_texture);
			extra_texture = nullptr;
			last_cx = 0;
			last_cy = 0;
			last_format = GS_UNKNOWN;
		}
		if (texture) {
			gs_texture_destroy(texture);
			texture = nullptr;
		}
		obs_leave_graphics();
	}

	inline void DestroyTransitionTexture()
	{
		obs_enter_graphics();
		if (transition_texture) {
			gs_texture_destroy(transition_texture);
			transition_texture = nullptr;
		}
		obs_leave_graphics();
	}

	/* ---------------------------- */

	bool CreateBrowser();
	void DestroyBrowser();
	void ExecuteOnBrowser(BrowserFunc func, bool async = false);

	/* ---------------------------- */

	BrowserSource(obs_data_t *settings, obs_source_t *source);
	~BrowserSource();

	void Destroy();

	void Update(obs_data_t *settings = nullptr);
	void Tick();
	void Render();
	uint64_t TransitionDurationNs() const;
	bool IsFadeTransition() const;
	bool IsCrossfadeTransition() const;
	void ResetTransition();
	bool CaptureTransitionTexture();
	void StartPlaylistTransition(size_t index);
	void StartPlaylistIndex(size_t index);

	void SendMouseClick(const struct obs_mouse_event *event, int32_t type, bool mouse_up, uint32_t click_count);
	void SendMouseMove(const struct obs_mouse_event *event, bool mouse_leave);
	void SendMouseWheel(const struct obs_mouse_event *event, int x_delta, int y_delta);
	void SendFocus(bool focus);
	void SendKeyClick(const struct obs_key_event *event, bool key_up);
	void SetShowing(bool showing);
	void SetActive(bool active);
	void Refresh();
	void PlayPause(bool pause);
	void Stop();
	void PlaylistNext();
	void PlaylistPrevious();
	void SetPlaylistIndex(size_t index);
	int64_t GetMediaDuration() const;
	int64_t GetMediaTime() const;
	void SetMediaTime(int64_t ms);
	enum obs_media_state GetMediaState() const;
	int GetPlaylistCount() const;
	int GetPlaylistIndex() const;
	std::string GetPlaylistItem(size_t index) const;

#if defined(BROWSER_EXTERNAL_BEGIN_FRAME_ENABLED) && defined(ENABLE_BROWSER_SHARED_TEXTURE)
	inline void SignalBeginFrame();
#endif

	void SetBrowser(CefRefPtr<CefBrowser> b);
	CefRefPtr<CefBrowser> GetBrowser();
};
