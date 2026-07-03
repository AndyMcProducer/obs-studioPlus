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

#include "obs-browser-source.hpp"
#include "browser-client.hpp"
#include "browser-scheme.hpp"
#include "wide-string.hpp"
#include <nlohmann/json.hpp>
#include <obs.hpp>
#include <util/platform.h>
#include <util/threading.h>
#include <QApplication>
#include <util/dstr.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <thread>
#include <mutex>

#ifdef __linux__
#include "linux-keyboard-helpers.hpp"
#endif

#ifdef ENABLE_BROWSER_QT_LOOP
#include <QEventLoop>
#include <QThread>
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
#include "drm-format.hpp"
#endif

using namespace std;

extern bool QueueCEFTask(std::function<void()> task);

static mutex browser_list_mutex;
static BrowserSource *first_browser = nullptr;
static constexpr const char *S_PLAYLIST = "playlist";
static constexpr const char *S_ALLOW_VIDEO = "allow_video";
static constexpr const char *S_VIDEO_SOURCE = "video_source";
static constexpr const char *S_VIDEO_RESOLUTION = "video_resolution";
static constexpr const char *BROWSER_PLAYLIST_SOURCE_ID = "browser_playlist_source";

static BrowserTransitionMode ClampBrowserTransitionMode(int mode)
{
	return mode < BROWSER_TRANSITION_CUT || mode > BROWSER_TRANSITION_CROSSFADE ? BROWSER_TRANSITION_CROSSFADE
										    : (BrowserTransitionMode)mode;
}

static float ClampFloat(float value, float min, float max)
{
	return value < min ? min : value > max ? max : value;
}

static bool BrowserSourceIsPlaylistSourceId(const char *id)
{
	return id && strcmp(id, BROWSER_PLAYLIST_SOURCE_ID) == 0;
}

static std::string LowerString(std::string str)
{
	std::transform(str.begin(), str.end(), str.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
	return str;
}

static void ParseBrowserVideoResolution(const std::string &resolution, int &width, int &height)
{
	width = 640;
	height = 480;

	const char *value = resolution.c_str();
	char *end = nullptr;
	long parsedWidth = strtol(value, &end, 10);
	if (!end || (*end != 'x' && *end != 'X'))
		return;

	char *heightEnd = nullptr;
	long parsedHeight = strtol(end + 1, &heightEnd, 10);
	if (heightEnd == end + 1 || parsedWidth <= 0 || parsedHeight <= 0)
		return;

	width = (int)parsedWidth;
	height = (int)parsedHeight;
}

static bool EndsWith(const std::string &value, const std::string &suffix)
{
	return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static bool PathIsUrl(const std::string &path)
{
	return path.find("://") != std::string::npos;
}

static std::string RewriteYouTubeUrl(std::string input)
{
	size_t schemeEnd = input.find("://");
	size_t hostStart = schemeEnd == std::string::npos ? 0 : schemeEnd + 3;
	size_t hostEnd = input.find_first_of("/?#", hostStart);
	std::string host = input.substr(hostStart, hostEnd == std::string::npos ? std::string::npos : hostEnd - hostStart);
	std::string hostLower = LowerString(host);

	if (hostLower == "youtu.be" || EndsWith(hostLower, ".youtu.be")) {
		size_t pathStart = hostEnd == std::string::npos ? std::string::npos : hostEnd + 1;
		size_t idEnd = pathStart == std::string::npos ? std::string::npos : input.find_first_of("?#/", pathStart);
		std::string videoId =
			pathStart == std::string::npos ? std::string() : input.substr(pathStart, idEnd - pathStart);
		std::string query;

		if (idEnd != std::string::npos && input[idEnd] == '?')
			query = input.substr(idEnd + 1);
		else if (idEnd != std::string::npos) {
			size_t queryStart = input.find('?', idEnd);
			if (queryStart != std::string::npos)
				query = input.substr(queryStart + 1);
		}

		if (videoId.empty())
			return input;

		std::string rewritten = "https://yout-ube.com/watch?v=" + videoId;
		if (!query.empty())
			rewritten += "&" + query;
		return rewritten;
	}

	if (hostLower == "youtube.com" || EndsWith(hostLower, ".youtube.com")) {
		size_t youtubePos = hostLower.rfind("youtube.com");
		std::string newHost = host;
		newHost.replace(youtubePos, strlen("youtube.com"), "yout-ube.com");
		input.replace(hostStart, host.size(), newHost);
	}

	return input;
}

static std::string NormalizeBrowserUrl(std::string input, bool isLocal, bool rewriteYouTube)
{
	if (isLocal && !input.empty()) {
		input = CefURIEncode(input, false);

#ifdef _WIN32
		size_t slash = input.find("%2F");
		size_t colon = input.find("%3A");

		if (slash != std::string::npos && colon != std::string::npos && colon < slash)
			input.replace(colon, 3, ":");
#endif

		while (input.find("%5C") != std::string::npos)
			input.replace(input.find("%5C"), 3, "/");

		while (input.find("%2F") != std::string::npos)
			input.replace(input.find("%2F"), 3, "/");

		// Local files are routed through our custom scheme handler to give them access to other local files.
		return "http://absolute/" + input;
	}

	return rewriteYouTube ? RewriteYouTubeUrl(input) : input;
}

static std::vector<std::string> LoadPlaylist(obs_data_t *settings)
{
	std::vector<std::string> playlist;
	obs_data_array_t *array = obs_data_get_array(settings, S_PLAYLIST);

	if (!array)
		return playlist;

	const size_t count = obs_data_array_count(array);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(array, i);
		const char *value = obs_data_get_string(item, "value");

		if (value && *value)
			playlist.emplace_back(value);

		obs_data_release(item);
	}

	obs_data_array_release(array);
	return playlist;
}

static void SendBrowserVisibility(CefRefPtr<CefBrowser> browser, bool isVisible)
{
	if (!browser)
		return;

	if (isVisible) {
		browser->GetHost()->WasResized();
		browser->GetHost()->WasHidden(false);
		browser->GetHost()->Invalidate(PET_VIEW);
	} else {
		browser->GetHost()->WasHidden(true);
	}

	CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("Visibility");
	CefRefPtr<CefListValue> args = msg->GetArgumentList();
	args->SetBool(0, isVisible);
	SendBrowserProcessMessage(browser, PID_RENDERER, msg);
}

void DispatchJSEvent(std::string eventName, std::string jsonString, BrowserSource *browser = nullptr);
static void SignalPlaylistUpdated(BrowserSource *bs);
static void SignalPlaylistSelectionChanged(BrowserSource *bs);

BrowserSource::BrowserSource(obs_data_t *, obs_source_t *source_) : source(source_)
{
	const char *id = obs_source_get_unversioned_id(source);
	playlist_source = BrowserSourceIsPlaylistSourceId(id);

	/* Register Refresh hotkey */
	auto refreshFunction = [](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
		if (pressed) {
			BrowserSource *bs = (BrowserSource *)data;
			bs->Refresh();
		}
	};

	obs_hotkey_register_source(source, "ObsBrowser.Refresh", obs_module_text("RefreshNoCache"), refreshFunction,
				   (void *)this);

	auto jsEventFunction = [](void *p, calldata_t *calldata) {
		const auto eventName = calldata_string(calldata, "eventName");
		if (!eventName)
			return;
		auto jsonString = calldata_string(calldata, "jsonString");
		if (!jsonString)
			jsonString = "null";
		DispatchJSEvent(eventName, jsonString, (BrowserSource *)p);
	};

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void javascript_event(string eventName, string jsonString)", jsEventFunction,
			 (void *)this);

	if (playlist_source) {
		auto getPlaylistCount = [](void *data, calldata_t *cd) {
			BrowserSource *bs = (BrowserSource *)data;
			calldata_set_int(cd, "count", bs->GetPlaylistCount());
		};

		auto getPlaylistIndex = [](void *data, calldata_t *cd) {
			BrowserSource *bs = (BrowserSource *)data;
			calldata_set_int(cd, "index", bs->GetPlaylistIndex());
		};

		auto getPlaylistItem = [](void *data, calldata_t *cd) {
			BrowserSource *bs = (BrowserSource *)data;
			int64_t index = calldata_int(cd, "index");
			std::string path = index >= 0 ? bs->GetPlaylistItem((size_t)index) : std::string();
			calldata_set_string(cd, "path", path.c_str());
		};

		auto setPlaylistIndex = [](void *data, calldata_t *cd) {
			BrowserSource *bs = (BrowserSource *)data;
			int64_t index = calldata_int(cd, "index");

			if (index >= 0)
				bs->SetPlaylistIndex((size_t)index);
		};

		proc_handler_add(ph, "void get_playlist_count(out int count)", getPlaylistCount, this);
		proc_handler_add(ph, "void get_playlist_index(out int index)", getPlaylistIndex, this);
		proc_handler_add(ph, "void get_playlist_item(in int index, out string path)", getPlaylistItem, this);
		proc_handler_add(ph, "void set_playlist_index(in int index)", setPlaylistIndex, this);

		signal_handler_t *sh = obs_source_get_signal_handler(source);
		signal_handler_add(sh, "void playlist_updated(int count)");
		signal_handler_add(sh, "void playlist_selection_changed(int index, string path)");
	}

	/* defer update */
	obs_source_update(source, nullptr);

	lock_guard<mutex> lock(browser_list_mutex);
	p_prev_next = &first_browser;
	next = first_browser;
	if (first_browser)
		first_browser->p_prev_next = &next;
	first_browser = this;
}

static void ActuallyCloseBrowser(CefRefPtr<CefBrowser> cefBrowser)
{
	CefRefPtr<CefClient> client = cefBrowser->GetHost()->GetClient();
	BrowserClient *bc = reinterpret_cast<BrowserClient *>(client.get());
	if (bc) {
		bc->bs = nullptr;
	}

	/*
         * This stops rendering
         * http://magpcss.org/ceforum/viewtopic.php?f=6&t=12079
         * https://bitbucket.org/chromiumembedded/cef/issues/1363/washidden-api-got-broken-on-branch-2062)
         */
	cefBrowser->GetHost()->WasHidden(true);
	cefBrowser->GetHost()->CloseBrowser(true);
}

BrowserSource::~BrowserSource()
{
	if (cefBrowser)
		ActuallyCloseBrowser(cefBrowser);
}

void BrowserSource::Destroy()
{
	destroying = true;
	DestroyTextures();
	DestroyTransitionTexture();
	DestroyVideoInputResources();

	lock_guard<mutex> lock(browser_list_mutex);
	if (next)
		next->p_prev_next = p_prev_next;
	*p_prev_next = next;

	QueueCEFTask([this]() { delete this; });
}

void BrowserSource::ExecuteOnBrowser(BrowserFunc func, bool async)
{
	if (!async) {
#ifdef ENABLE_BROWSER_QT_LOOP
		if (QThread::currentThread() == qApp->thread()) {
			if (!!cefBrowser)
				func(cefBrowser);
			return;
		}
#endif
		os_event_t *finishedEvent;
		os_event_init(&finishedEvent, OS_EVENT_TYPE_AUTO);
		bool success = QueueCEFTask([&]() {
			if (!!cefBrowser)
				func(cefBrowser);
			os_event_signal(finishedEvent);
		});
		if (success) {
			os_event_wait(finishedEvent);
		}
		os_event_destroy(finishedEvent);
	} else {
		CefRefPtr<CefBrowser> browser = GetBrowser();
		if (!!browser) {
#ifdef ENABLE_BROWSER_QT_LOOP
			QueueBrowserTask(cefBrowser, func);
#else
			QueueCEFTask([=]() { func(browser); });
#endif
		}
	}
}

bool BrowserSource::CreateBrowser()
{
	return QueueCEFTask([this]() {
#ifdef ENABLE_BROWSER_SHARED_TEXTURE
		if (hwaccel) {
			obs_enter_graphics();
#if defined(__APPLE__) || defined(_WIN32)
			tex_sharing_avail = gs_shared_texture_available();
#else
			tex_sharing_avail = obs_cef_all_drm_formats_supported();
#endif
			obs_leave_graphics();
		}
#else
		bool hwaccel = false;
#endif

		CefRefPtr<BrowserClient> browserClient =
			new BrowserClient(this, hwaccel && tex_sharing_avail, reroute_audio, allow_mic, mic_device, allow_video,
					  video_source, video_input_width, video_input_height, webpage_control_level);

		CefWindowInfo windowInfo;
		windowInfo.bounds.width = width;
		windowInfo.bounds.height = height;
		windowInfo.windowless_rendering_enabled = true;

#ifdef ENABLE_BROWSER_SHARED_TEXTURE
		windowInfo.shared_texture_enabled = hwaccel;
#endif

		CefBrowserSettings cefBrowserSettings;

#ifdef ENABLE_BROWSER_SHARED_TEXTURE
#ifdef BROWSER_EXTERNAL_BEGIN_FRAME_ENABLED
		if (!fps_custom) {
			windowInfo.external_begin_frame_enabled = true;
			cefBrowserSettings.windowless_frame_rate = 0;
		} else {
			cefBrowserSettings.windowless_frame_rate = fps;
		}
#else
		struct obs_video_info ovi;
		obs_get_video_info(&ovi);
		canvas_fps = (double)ovi.fps_num / (double)ovi.fps_den;
		cefBrowserSettings.windowless_frame_rate = (fps_custom) ? fps : canvas_fps;
#endif
#else
		cefBrowserSettings.windowless_frame_rate = fps;
#endif

		cefBrowserSettings.default_font_size = 16;
		cefBrowserSettings.default_fixed_font_size = 16;

		auto browser = CefBrowserHost::CreateBrowserSync(windowInfo, browserClient, url, cefBrowserSettings,
								 CefRefPtr<CefDictionaryValue>(), nullptr);

		SetBrowser(browser);

		if (reroute_audio)
			cefBrowser->GetHost()->SetAudioMuted(true);
		if (obs_source_showing(source))
			is_showing = true;

		SendBrowserVisibility(cefBrowser, is_showing);
	});
}

void BrowserSource::DestroyBrowser()
{
	ExecuteOnBrowser(ActuallyCloseBrowser, true);
	SetBrowser(nullptr);
}

void BrowserSource::SendMouseClick(const struct obs_mouse_event *event, int32_t type, bool mouse_up,
				   uint32_t click_count)
{
	uint32_t modifiers = event->modifiers;
	int32_t x = event->x;
	int32_t y = event->y;

	ExecuteOnBrowser(
		[=](CefRefPtr<CefBrowser> cefBrowser) {
			CefMouseEvent e;
			e.modifiers = modifiers;
			e.x = x;
			e.y = y;
			CefBrowserHost::MouseButtonType buttonType = (CefBrowserHost::MouseButtonType)type;
			cefBrowser->GetHost()->SendMouseClickEvent(e, buttonType, mouse_up, click_count);
		},
		true);
}

void BrowserSource::SendMouseMove(const struct obs_mouse_event *event, bool mouse_leave)
{
	uint32_t modifiers = event->modifiers;
	int32_t x = event->x;
	int32_t y = event->y;

	ExecuteOnBrowser(
		[=](CefRefPtr<CefBrowser> cefBrowser) {
			CefMouseEvent e;
			e.modifiers = modifiers;
			e.x = x;
			e.y = y;
			cefBrowser->GetHost()->SendMouseMoveEvent(e, mouse_leave);
		},
		true);
}

void BrowserSource::SendMouseWheel(const struct obs_mouse_event *event, int x_delta, int y_delta)
{
	uint32_t modifiers = event->modifiers;
	int32_t x = event->x;
	int32_t y = event->y;

	ExecuteOnBrowser(
		[=](CefRefPtr<CefBrowser> cefBrowser) {
			CefMouseEvent e;
			e.modifiers = modifiers;
			e.x = x;
			e.y = y;
			cefBrowser->GetHost()->SendMouseWheelEvent(e, x_delta, y_delta);
		},
		true);
}

void BrowserSource::SendFocus(bool focus)
{
	ExecuteOnBrowser([=](CefRefPtr<CefBrowser> cefBrowser) { cefBrowser->GetHost()->SetFocus(focus); }, true);
}

void BrowserSource::SendKeyClick(const struct obs_key_event *event, bool key_up)
{
	if (destroying)
		return;

	std::string text = event->text;
#ifdef __linux__
	uint32_t native_vkey = KeyboardCodeFromXKeysym(event->native_vkey);
	uint32_t modifiers = event->native_modifiers;
#elif defined(_WIN32) || defined(__APPLE__)
	uint32_t native_vkey = event->native_vkey;
	uint32_t modifiers = event->modifiers;
#else
	uint32_t native_vkey = event->native_vkey;
	uint32_t native_scancode = event->native_scancode;
	uint32_t modifiers = event->native_modifiers;
#endif

	ExecuteOnBrowser(
		[=](CefRefPtr<CefBrowser> cefBrowser) {
			CefKeyEvent e;
			e.windows_key_code = native_vkey;
#ifdef __APPLE__
			e.native_key_code = native_vkey;
#endif

			e.type = key_up ? KEYEVENT_KEYUP : KEYEVENT_RAWKEYDOWN;

			if (!text.empty()) {
				wstring wide = to_wide(text);
				if (wide.size())
					e.character = wide[0];
			}

			//e.native_key_code = native_vkey;
			e.modifiers = modifiers;

			cefBrowser->GetHost()->SendKeyEvent(e);
			if (!text.empty() && !key_up) {
				e.type = KEYEVENT_CHAR;
#ifdef __linux__
				e.windows_key_code = KeyboardCodeFromXKeysym(e.character);
#elif defined(_WIN32)
				e.windows_key_code = e.character;
#elif !defined(__APPLE__)
				e.native_key_code = native_scancode;
#endif
				cefBrowser->GetHost()->SendKeyEvent(e);
			}
		},
		true);
}

void BrowserSource::SetShowing(bool showing)
{
	if (destroying)
		return;

	is_showing = showing;

	if (shutdown_on_invisible) {
		if (showing) {
			Update();
		} else {
			DestroyBrowser();
		}
	} else {
		ExecuteOnBrowser(
			[=](CefRefPtr<CefBrowser> cefBrowser) {
				CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("Visibility");
				CefRefPtr<CefListValue> args = msg->GetArgumentList();
				args->SetBool(0, showing);
				SendBrowserProcessMessage(cefBrowser, PID_RENDERER, msg);
			},
			true);
		nlohmann::json json;
		json["visible"] = showing;
		DispatchJSEvent("obsSourceVisibleChanged", json.dump(), this);
#if defined(BROWSER_EXTERNAL_BEGIN_FRAME_ENABLED) && defined(ENABLE_BROWSER_SHARED_TEXTURE)
		if (showing && !fps_custom) {
			reset_frame = false;
		}
#endif

		SendBrowserVisibility(cefBrowser, showing);

		if (showing)
			return;

		obs_enter_graphics();

		if (!hwaccel && texture) {
			DestroyTextures();
		}

		obs_leave_graphics();
	}
}

void BrowserSource::SetActive(bool active)
{
	ExecuteOnBrowser(
		[=](CefRefPtr<CefBrowser> cefBrowser) {
			CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("Active");
			CefRefPtr<CefListValue> args = msg->GetArgumentList();
			args->SetBool(0, active);
			SendBrowserProcessMessage(cefBrowser, PID_RENDERER, msg);
		},
		true);
	nlohmann::json json;
	json["active"] = active;
	DispatchJSEvent("obsSourceActiveChanged", json.dump(), this);
}

void BrowserSource::Refresh()
{
	media_state = OBS_MEDIA_STATE_PLAYING;
	ExecuteOnBrowser([](CefRefPtr<CefBrowser> cefBrowser) { cefBrowser->ReloadIgnoreCache(); }, true);
}

void BrowserSource::PlayPause(bool pause)
{
	media_state = pause ? OBS_MEDIA_STATE_PAUSED : OBS_MEDIA_STATE_PLAYING;
	ExecuteOnBrowser(
		[=](CefRefPtr<CefBrowser> cefBrowser) {
			const char *script = pause
						     ? "document.querySelectorAll('video,audio').forEach((media) => media.pause());"
						     : "document.querySelectorAll('video,audio').forEach((media) => {"
						       "const promise = media.play();"
						       "if (promise && promise.catch) promise.catch(() => {});"
						       "});";
			CefRefPtr<CefFrame> frame = cefBrowser->GetMainFrame();
			frame->ExecuteJavaScript(script, frame->GetURL(), 0);
		},
		true);
}

void BrowserSource::Stop()
{
	media_state = OBS_MEDIA_STATE_STOPPED;
	ExecuteOnBrowser(
		[](CefRefPtr<CefBrowser> cefBrowser) {
			CefRefPtr<CefFrame> frame = cefBrowser->GetMainFrame();
			frame->ExecuteJavaScript("document.querySelectorAll('video,audio').forEach((media) => media.pause());",
						 frame->GetURL(), 0);
			cefBrowser->StopLoad();
		},
		true);
}

void BrowserSource::PlaylistNext()
{
	if (playlist.empty())
		return;

	if (playlist_index + 1 < playlist.size()) {
		SetPlaylistIndex(playlist_index + 1);
	} else if (playlist_looping) {
		SetPlaylistIndex(0);
	}
}

void BrowserSource::PlaylistPrevious()
{
	if (playlist.empty())
		return;

	if (playlist_index > 0) {
		SetPlaylistIndex(playlist_index - 1);
	} else if (playlist_looping) {
		SetPlaylistIndex(playlist.size() - 1);
	}
}

uint64_t BrowserSource::VideoInputFrameIntervalNs() const
{
	int frameRate = fps > 0 ? fps : 30;
	if (frameRate < 1)
		frameRate = 1;
	else if (frameRate > 60)
		frameRate = 60;
	return 1000000000ULL / (uint64_t)frameRate;
}

bool BrowserSource::EnsureVideoInputResources()
{
	if (video_input_width <= 0 || video_input_height <= 0)
		return false;

	const size_t frameSize = (size_t)video_input_width * (size_t)video_input_height * 4;
	if (video_input_render && video_input_stage && video_input_frame.size() == frameSize)
		return true;

	if (video_input_stage) {
		gs_stagesurface_destroy(video_input_stage);
		video_input_stage = nullptr;
	}
	if (video_input_render) {
		gs_texrender_destroy(video_input_render);
		video_input_render = nullptr;
	}

	video_input_render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	video_input_stage = gs_stagesurface_create((uint32_t)video_input_width, (uint32_t)video_input_height, GS_RGBA);
	video_input_frame.assign(frameSize, 0);

	return video_input_render && video_input_stage && !video_input_frame.empty();
}

void BrowserSource::SendVideoInputFrame()
{
	if (video_input_frame.empty())
		return;

	const int frameWidth = video_input_width;
	const int frameHeight = video_input_height;
	std::vector<uint8_t> frame = video_input_frame;

	ExecuteOnBrowser(
		[frame = std::move(frame), frameWidth, frameHeight](CefRefPtr<CefBrowser> cefBrowser) {
			CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("ObsBrowserVideoFrame");
			CefRefPtr<CefListValue> args = msg->GetArgumentList();
			args->SetInt(0, frameWidth);
			args->SetInt(1, frameHeight);
			args->SetBinary(2, CefBinaryValue::Create(frame.data(), frame.size()));
			SendBrowserProcessMessage(cefBrowser, PID_RENDERER, msg);
		},
		true);
}

void BrowserSource::StopVideoInputFrame()
{
	ExecuteOnBrowser(
		[](CefRefPtr<CefBrowser> cefBrowser) {
			CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("ObsBrowserVideoStop");
			SendBrowserProcessMessage(cefBrowser, PID_RENDERER, msg);
		},
		true);
}

void BrowserSource::RenderVideoInputFrame()
{
	if (!allow_video || video_source.empty() || destroying)
		return;

	const uint64_t now = os_gettime_ns();
	const uint64_t interval = VideoInputFrameIntervalNs();
	if (last_video_input_frame_ns && now - last_video_input_frame_ns < interval)
		return;
	last_video_input_frame_ns = now;

	OBSSourceAutoRelease input = obs_get_source_by_name(video_source.c_str());
	if (!input || input == source)
		return;
	if ((obs_source_get_output_flags(input) & OBS_SOURCE_VIDEO) == 0)
		return;
	if (!EnsureVideoInputResources())
		return;

	const uint32_t targetWidth = (uint32_t)video_input_width;
	const uint32_t targetHeight = (uint32_t)video_input_height;
	const uint32_t sourceWidth = obs_source_get_width(input);
	const uint32_t sourceHeight = obs_source_get_height(input);
	if (!sourceWidth || !sourceHeight)
		return;

	gs_texrender_reset(video_input_render);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	bool rendered = false;
	if (gs_texrender_begin_with_color_space(video_input_render, targetWidth, targetHeight, GS_CS_SRGB)) {
		struct vec4 clearColor;
		vec4_zero(&clearColor);
		gs_clear(GS_CLEAR_COLOR, &clearColor, 0.0f, 0);
		gs_ortho(0.0f, (float)targetWidth, 0.0f, (float)targetHeight, -100.0f, 100.0f);

		const float scale = std::min((float)targetWidth / (float)sourceWidth,
					     (float)targetHeight / (float)sourceHeight);
		const float drawWidth = (float)sourceWidth * scale;
		const float drawHeight = (float)sourceHeight * scale;
		const float x = ((float)targetWidth - drawWidth) * 0.5f;
		const float y = ((float)targetHeight - drawHeight) * 0.5f;

		gs_matrix_push();
		gs_matrix_translate3f(x, y, 0.0f);
		gs_matrix_scale3f(scale, scale, 1.0f);
		obs_source_video_render(input);
		gs_matrix_pop();

		gs_texrender_end(video_input_render);
		rendered = true;
	}

	gs_blend_state_pop();
	if (!rendered)
		return;

	gs_texture_t *texture = gs_texrender_get_texture(video_input_render);
	if (!texture)
		return;

	gs_stage_texture(video_input_stage, texture);

	uint8_t *frameData = nullptr;
	uint32_t frameLinesize = 0;
	if (!gs_stagesurface_map(video_input_stage, &frameData, &frameLinesize))
		return;

	const size_t rowBytes = (size_t)targetWidth * 4;
	for (uint32_t y = 0; y < targetHeight; y++) {
		memcpy(video_input_frame.data() + (size_t)y * rowBytes, frameData + (size_t)y * frameLinesize, rowBytes);
	}
	gs_stagesurface_unmap(video_input_stage);

	SendVideoInputFrame();
}

uint64_t BrowserSource::TransitionDurationNs() const
{
	return transition_ms > 0 ? (uint64_t)transition_ms * 1000000ULL : 0;
}

bool BrowserSource::IsFadeTransition() const
{
	return transition_mode == BROWSER_TRANSITION_FADE && transition_ms > 0;
}

bool BrowserSource::IsCrossfadeTransition() const
{
	return transition_mode == BROWSER_TRANSITION_CROSSFADE && transition_ms > 0;
}

void BrowserSource::ResetTransition()
{
	transition_active = false;
	transition_midpoint_pending = false;
	transition_start_ns = 0;
	DestroyTransitionTexture();
}

bool BrowserSource::CaptureTransitionTexture()
{
	DestroyTransitionTexture();

	if (!texture)
		return false;

	obs_enter_graphics();

	const uint32_t cx = gs_texture_get_width(texture);
	const uint32_t cy = gs_texture_get_height(texture);
	const gs_color_format format = gs_texture_get_color_format(texture);

	if (!cx || !cy || format == GS_UNKNOWN) {
		obs_leave_graphics();
		return false;
	}

	transition_texture = gs_texture_create(cx, cy, format, 1, nullptr, 0);
	if (transition_texture)
		gs_copy_texture(transition_texture, texture);

	obs_leave_graphics();
	return transition_texture != nullptr;
}

void BrowserSource::StartPlaylistIndex(size_t index)
{
	if (!playlist_source || index >= playlist.size())
		return;

	const std::string rawUrl = playlist[index];
	const bool n_is_local = !PathIsUrl(rawUrl);
	const std::string n_url = NormalizeBrowserUrl(rawUrl, n_is_local, rewrite_youtube);

	playlist_index = index;
	media_state = OBS_MEDIA_STATE_PLAYING;
	is_local = n_is_local;
	url = n_url;

	DestroyBrowser();
	DestroyTextures();

	if (!shutdown_on_invisible || obs_source_showing(source))
		create_browser = true;

	SignalPlaylistSelectionChanged(this);
	obs_source_media_started(source);
}

void BrowserSource::StartPlaylistTransition(size_t index)
{
	if (!playlist_source || index >= playlist.size())
		return;

	if (!IsFadeTransition() && !IsCrossfadeTransition()) {
		ResetTransition();
		StartPlaylistIndex(index);
		return;
	}

	ResetTransition();
	pending_playlist_index = index;

	if (IsCrossfadeTransition()) {
		if (!CaptureTransitionTexture()) {
			StartPlaylistIndex(index);
			return;
		}

		transition_active = true;
		transition_midpoint_pending = false;
		transition_start_ns = 0;
		StartPlaylistIndex(index);
		return;
	}

	transition_active = true;
	transition_midpoint_pending = texture != nullptr;
	transition_start_ns = transition_midpoint_pending ? os_gettime_ns() : 0;

	if (!transition_midpoint_pending)
		StartPlaylistIndex(index);
}

void BrowserSource::SetPlaylistIndex(size_t index)
{
	if (!playlist_source || index >= playlist.size())
		return;

	const std::string rawUrl = playlist[index];
	const bool n_is_local = !PathIsUrl(rawUrl);
	const std::string n_url = NormalizeBrowserUrl(rawUrl, n_is_local, rewrite_youtube);
	const bool reload = index != playlist_index || n_url != url || n_is_local != is_local;

	if (reload) {
		StartPlaylistTransition(index);
		return;
	}

	playlist_index = index;
	media_state = OBS_MEDIA_STATE_PLAYING;

	SignalPlaylistSelectionChanged(this);
	obs_source_media_started(source);
}

int64_t BrowserSource::GetMediaDuration() const
{
	return 0;
}

int64_t BrowserSource::GetMediaTime() const
{
	return 0;
}

void BrowserSource::SetMediaTime(int64_t ms)
{
	ExecuteOnBrowser(
		[=](CefRefPtr<CefBrowser> cefBrowser) {
			std::string script =
				std::string("document.querySelectorAll('video,audio').forEach((media) => {"
					    "try { media.currentTime = ") +
				std::to_string((double)ms / 1000.0) + "; } catch (e) {}"
								      "});";
			CefRefPtr<CefFrame> frame = cefBrowser->GetMainFrame();
			frame->ExecuteJavaScript(script, frame->GetURL(), 0);
		},
		true);
}

enum obs_media_state BrowserSource::GetMediaState() const
{
	return media_state;
}

int BrowserSource::GetPlaylistCount() const
{
	return (int)playlist.size();
}

int BrowserSource::GetPlaylistIndex() const
{
	return playlist.empty() ? -1 : (int)playlist_index;
}

std::string BrowserSource::GetPlaylistItem(size_t index) const
{
	return index < playlist.size() ? playlist[index] : std::string();
}

void BrowserSource::SetBrowser(CefRefPtr<CefBrowser> b)
{
	std::lock_guard<std::recursive_mutex> auto_lock(lockBrowser);
	cefBrowser = b;
}

CefRefPtr<CefBrowser> BrowserSource::GetBrowser()
{
	std::lock_guard<std::recursive_mutex> auto_lock(lockBrowser);
	return cefBrowser;
}

#ifdef ENABLE_BROWSER_SHARED_TEXTURE
#ifdef BROWSER_EXTERNAL_BEGIN_FRAME_ENABLED
inline void BrowserSource::SignalBeginFrame()
{
	if (reset_frame) {
		ExecuteOnBrowser(
			[](CefRefPtr<CefBrowser> cefBrowser) { cefBrowser->GetHost()->SendExternalBeginFrame(); },
			true);

		reset_frame = false;
	}
}
#endif
#endif

static void SignalPlaylistUpdated(BrowserSource *bs)
{
	signal_handler_t *sh = obs_source_get_signal_handler(bs->source);
	calldata_t cd = {};

	calldata_set_int(&cd, "count", bs->GetPlaylistCount());
	signal_handler_signal(sh, "playlist_updated", &cd);
	calldata_free(&cd);
}

static void SignalPlaylistSelectionChanged(BrowserSource *bs)
{
	signal_handler_t *sh = obs_source_get_signal_handler(bs->source);
	calldata_t cd = {};
	int index = bs->GetPlaylistIndex();
	std::string path = index >= 0 ? bs->GetPlaylistItem((size_t)index) : std::string();

	calldata_set_int(&cd, "index", index);
	calldata_set_string(&cd, "path", path.c_str());
	signal_handler_signal(sh, "playlist_selection_changed", &cd);
	calldata_free(&cd);
}

void BrowserSource::Update(obs_data_t *settings)
{
	if (settings) {
		bool n_is_local;
		int n_width;
		int n_height;
		bool n_fps_custom;
		int n_fps;
		bool n_shutdown;
		bool n_restart;
		bool n_reroute;
		bool n_allow_mic;
		bool n_allow_video;
		int n_video_input_width;
		int n_video_input_height;
		ControlLevel n_webpage_control_level;
		bool n_playlist_looping = false;
		bool n_rewrite_youtube = false;
		BrowserTransitionMode n_transition_mode = transition_mode;
		int n_transition_ms = transition_ms;
		std::string n_url;
		std::string n_raw_url;
		std::string n_css;
		std::string n_mic_device;
		std::string n_video_source;
		std::string n_video_resolution;
		std::vector<std::string> n_playlist;
		bool playlist_changed = false;
		bool playlist_selection_changed = false;

		n_is_local = obs_data_get_bool(settings, "is_local_file");
		n_width = (int)obs_data_get_int(settings, "width");
		n_height = (int)obs_data_get_int(settings, "height");
		n_fps_custom = obs_data_get_bool(settings, "fps_custom");
		n_fps = (int)obs_data_get_int(settings, "fps");
		n_shutdown = obs_data_get_bool(settings, "shutdown");
		n_restart = obs_data_get_bool(settings, "restart_when_active");
		n_css = obs_data_get_string(settings, "css");
		n_raw_url = obs_data_get_string(settings, n_is_local ? "local_file" : "url");
		n_reroute = obs_data_get_bool(settings, "reroute_audio");
		n_allow_mic = obs_data_get_bool(settings, "allow_mic");
		n_mic_device = obs_data_get_string(settings, "mic_device");
		if (n_mic_device.empty())
			n_mic_device = "default";
		n_allow_video = obs_data_get_bool(settings, S_ALLOW_VIDEO);
		n_video_source = obs_data_get_string(settings, S_VIDEO_SOURCE);
		n_video_resolution = obs_data_get_string(settings, S_VIDEO_RESOLUTION);
		ParseBrowserVideoResolution(n_video_resolution, n_video_input_width, n_video_input_height);
		n_webpage_control_level =
			static_cast<ControlLevel>(obs_data_get_int(settings, "webpage_control_level"));

		if (playlist_source) {
			std::string oldActiveItem;
			size_t oldPlaylistIndex = playlist_index;

			if (!playlist.empty() && playlist_index < playlist.size())
				oldActiveItem = playlist[playlist_index];

			n_playlist = LoadPlaylist(settings);
			n_playlist_looping = obs_data_get_bool(settings, "looping");
			n_rewrite_youtube = obs_data_get_bool(settings, "rewrite_youtube");
			n_transition_mode = ClampBrowserTransitionMode((int)obs_data_get_int(settings, "transition_mode"));
			n_transition_ms = (int)obs_data_get_int(settings, "transition_ms");
			if (n_transition_ms < 0)
				n_transition_ms = 0;
			playlist_changed = n_playlist != playlist;
			playlist_selection_changed = n_rewrite_youtube != rewrite_youtube;

			if (playlist_changed && !oldActiveItem.empty()) {
				auto it = std::find(n_playlist.begin(), n_playlist.end(), oldActiveItem);

				if (it != n_playlist.end())
					playlist_index = (size_t)std::distance(n_playlist.begin(), it);
				else if (playlist_index >= n_playlist.size())
					playlist_index = 0;
			} else if (playlist_index >= n_playlist.size()) {
				playlist_index = 0;
			}

			playlist = n_playlist;
			playlist_looping = n_playlist_looping;
			rewrite_youtube = n_rewrite_youtube;
			transition_mode = n_transition_mode;
			transition_ms = n_transition_ms;
			if (!IsFadeTransition() && !IsCrossfadeTransition())
				ResetTransition();

			if (!playlist.empty()) {
				n_raw_url = playlist[playlist_index];
				n_is_local = !PathIsUrl(n_raw_url);
			}

			playlist_selection_changed |= oldPlaylistIndex != playlist_index;
		}

		n_url = NormalizeBrowserUrl(n_raw_url, n_is_local, playlist_source && rewrite_youtube);

		if (n_is_local == is_local && n_fps_custom == fps_custom && n_fps == fps &&
		    n_shutdown == shutdown_on_invisible && n_restart == restart && n_css == css && n_url == url &&
		    n_reroute == reroute_audio && n_allow_mic == allow_mic && n_mic_device == mic_device &&
		    n_allow_video == allow_video && n_video_source == video_source &&
		    n_video_input_width == video_input_width && n_video_input_height == video_input_height &&
		    n_webpage_control_level == webpage_control_level) {
			if (playlist_source) {
				if (playlist_changed)
					SignalPlaylistUpdated(this);
				if (playlist_changed || playlist_selection_changed)
					SignalPlaylistSelectionChanged(this);
			}

			if (n_width == width && n_height == height)
				return;

			width = n_width;
			height = n_height;
			ExecuteOnBrowser(
				[=](CefRefPtr<CefBrowser> cefBrowser) {
					const CefSize cefSize(width, height);
					cefBrowser->GetHost()->GetClient()->GetDisplayHandler()->OnAutoResize(
						cefBrowser, cefSize);
					cefBrowser->GetHost()->WasResized();
					cefBrowser->GetHost()->Invalidate(PET_VIEW);
				},
				true);
			return;
		}

		is_local = n_is_local;
		width = n_width;
		height = n_height;
		fps = n_fps;
		fps_custom = n_fps_custom;
		shutdown_on_invisible = n_shutdown;
		reroute_audio = n_reroute;
		allow_mic = n_allow_mic;
		mic_device = n_mic_device;
		allow_video = n_allow_video;
		video_source = n_video_source;
		video_input_width = n_video_input_width;
		video_input_height = n_video_input_height;
		webpage_control_level = n_webpage_control_level;
		restart = n_restart;
		css = n_css;
		url = n_url;

		obs_source_set_audio_active(source, reroute_audio);

		if (playlist_source) {
			if (playlist_changed)
				SignalPlaylistUpdated(this);
			if (playlist_changed || playlist_selection_changed)
				SignalPlaylistSelectionChanged(this);
		}
	}

	DestroyBrowser();
	DestroyTextures();
	ResetTransition();
	DestroyVideoInputResources();
	last_video_input_frame_ns = 0;

	if (!shutdown_on_invisible || obs_source_showing(source))
		create_browser = true;

	first_update = false;
}

void BrowserSource::Tick()
{
	if (transition_active) {
		const uint64_t now = os_gettime_ns();
		const uint64_t durationNs = TransitionDurationNs();
		if (!durationNs) {
			ResetTransition();
		} else if (transition_start_ns == 0) {
			if (texture) {
				transition_start_ns = now;
				if (IsFadeTransition() && !transition_midpoint_pending)
					transition_start_ns -= durationNs / 2;
			}
		} else {
			const uint64_t elapsed = now > transition_start_ns ? now - transition_start_ns : 0;

			if (IsFadeTransition() && transition_midpoint_pending && elapsed >= durationNs / 2) {
				transition_midpoint_pending = false;
				StartPlaylistIndex(pending_playlist_index);
				transition_start_ns = 0;
			} else if (elapsed >= durationNs) {
				ResetTransition();
			}
		}
	}

	if (create_browser && CreateBrowser())
		create_browser = false;
#if defined(ENABLE_BROWSER_SHARED_TEXTURE)
#if defined(BROWSER_EXTERNAL_BEGIN_FRAME_ENABLED)
	if (!fps_custom)
		reset_frame = true;
#else
	struct obs_video_info ovi;
	obs_get_video_info(&ovi);
	double video_fps = (double)ovi.fps_num / (double)ovi.fps_den;

	if (!fps_custom) {
		if (!!cefBrowser && canvas_fps != video_fps) {
			cefBrowser->GetHost()->SetWindowlessFrameRate(video_fps);
			canvas_fps = video_fps;
		}
	}
#endif
#endif
}

extern void ProcessCef();

static gs_effect_t *GetBrowserEffect()
{
#ifdef __APPLE__
	int type = gs_get_device_type();

	if (type == GS_DEVICE_OPENGL)
		return obs_get_base_effect((hwaccel) ? OBS_EFFECT_DEFAULT_RECT : OBS_EFFECT_DEFAULT);
#endif

	return obs_get_base_effect(OBS_EFFECT_DEFAULT);
}

static void DrawBrowserTexture(BrowserSource *bs, gs_texture_t *inputTexture, float opacity, bool flip,
			       bool useExtraTexture)
{
	if (!inputTexture || opacity <= 0.0f)
		return;

	gs_effect_t *effect = GetBrowserEffect();
	bool linearSample = true;
	gs_texture_t *drawTexture = inputTexture;

	if (useExtraTexture) {
		linearSample = bs->extra_texture == nullptr;
		if (!linearSample && !obs_source_get_texcoords_centered(bs->source)) {
			gs_copy_texture(bs->extra_texture, inputTexture);
			drawTexture = bs->extra_texture;
			linearSample = true;
		}
	}

	gs_eparam_t *const image = gs_effect_get_param_by_name(effect, "image");
	gs_eparam_t *const opacityParam = gs_effect_get_param_by_name(effect, "opacity");
	const bool forceOpacityTech = opacityParam && opacity < 0.999f;

	if (opacityParam)
		gs_effect_set_float(opacityParam, opacity);

	const char *tech;
	if (forceOpacityTech || linearSample) {
		gs_effect_set_texture_srgb(image, drawTexture);
		tech = "Draw";
	} else {
		gs_effect_set_texture(image, drawTexture);
		tech = "DrawSrgbDecompress";
	}

	const uint32_t flipFlag = flip ? GS_FLIP_V : 0;
	while (gs_effect_loop(effect, tech))
		gs_draw_sprite(drawTexture, flipFlag, 0, 0);

	if (opacityParam)
		gs_effect_set_float(opacityParam, 1.0f);
}

void BrowserSource::Render()
{
	RenderVideoInputFrame();

	bool flip = false;
	float currentOpacity = 1.0f;
	float transitionOpacity = 0.0f;
#if defined(ENABLE_BROWSER_SHARED_TEXTURE) && CHROME_VERSION_BUILD < 6367
	flip = hwaccel;
#endif

	if (playlist_source && transition_active) {
		const uint64_t durationNs = TransitionDurationNs();
		const uint64_t now = os_gettime_ns();
		if (transition_start_ns == 0) {
			if (IsCrossfadeTransition()) {
				transitionOpacity = 1.0f;
				currentOpacity = 0.0f;
			} else if (IsFadeTransition() && !transition_midpoint_pending) {
				currentOpacity = 0.0f;
			}
		} else {
			const uint64_t elapsed = now > transition_start_ns ? now - transition_start_ns : 0;
			const float progress =
				durationNs ? ClampFloat((float)elapsed / (float)durationNs, 0.0f, 1.0f) : 1.0f;

			if (IsCrossfadeTransition()) {
				transitionOpacity = 1.0f - progress;
				currentOpacity = progress;
			} else if (IsFadeTransition()) {
				currentOpacity = progress < 0.5f ? 1.0f - progress * 2.0f : (progress - 0.5f) * 2.0f;
			}
		}
	}

	if (texture || transition_texture) {
		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(true);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

		DrawBrowserTexture(this, transition_texture, transitionOpacity, flip, false);
		DrawBrowserTexture(this, texture, currentOpacity, flip, true);

		gs_blend_state_pop();
		gs_enable_framebuffer_srgb(previous);
	}

#if defined(BROWSER_EXTERNAL_BEGIN_FRAME_ENABLED) && defined(ENABLE_BROWSER_SHARED_TEXTURE)
	SignalBeginFrame();
#elif defined(ENABLE_BROWSER_QT_LOOP)
	ProcessCef();
#endif
}

static void ExecuteOnBrowser(BrowserFunc func, BrowserSource *bs)
{
	lock_guard<mutex> lock(browser_list_mutex);

	if (bs) {
		BrowserSource *bsw = reinterpret_cast<BrowserSource *>(bs);
		bsw->ExecuteOnBrowser(func, true);
	}
}

static void ExecuteOnAllBrowsers(BrowserFunc func)
{
	lock_guard<mutex> lock(browser_list_mutex);

	BrowserSource *bs = first_browser;
	while (bs) {
		BrowserSource *bsw = reinterpret_cast<BrowserSource *>(bs);
		bsw->ExecuteOnBrowser(func, true);
		bs = bs->next;
	}
}

void DispatchJSEvent(std::string eventName, std::string jsonString, BrowserSource *browser)
{
	const auto jsEvent = [=](CefRefPtr<CefBrowser> cefBrowser) {
		CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("DispatchJSEvent");
		CefRefPtr<CefListValue> args = msg->GetArgumentList();

		args->SetString(0, eventName);
		args->SetString(1, jsonString);
		SendBrowserProcessMessage(cefBrowser, PID_RENDERER, msg);
	};

	if (!browser)
		ExecuteOnAllBrowsers(jsEvent);
	else
		ExecuteOnBrowser(jsEvent, browser);
}
