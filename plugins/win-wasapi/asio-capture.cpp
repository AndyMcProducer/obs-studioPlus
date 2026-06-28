#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>

#include <atomic>
#include <string>

#include "asio-capture.hpp"
#include "../../libobs/media-io/asio/obs-asio-bridge.h"
#include "../../libobs/media-io/asio/obs-asio.h"

using std::string;

static constexpr const char *OPT_CHANNEL_ID = "channel_id";
static constexpr const char *OPT_DEVICE_ID = "device_id";
static constexpr const char *ASIO_DEVICE_PREFIX = "asio:";

static config_t *GetAsioProfileConfig()
{
	return obs_frontend_get_profile_config();
}

static bool IsAsioProfileSelected(config_t *profile)
{
	const char *backend;

	if (!profile)
		return false;

	backend = config_get_string(profile, "Audio", "Backend");
	return backend && strcmp(backend, "ASIO") == 0;
}

static void SyncAsioSelectedDeviceFromProfile()
{
	config_t *profile = GetAsioProfileConfig();
	const char *device = profile ? config_get_string(profile, "Audio", "ASIODevice") : nullptr;

	obs_asio_set_selected_device(device && *device ? device : nullptr);
}

bool IsASIOCaptureSelection(const char *value)
{
	return value && strncmp(value, ASIO_DEVICE_PREFIX, strlen(ASIO_DEVICE_PREFIX)) == 0;
}

bool IsASIOCaptureBackendActive()
{
	struct obs_audio_info2 audioInfo = {};
	if (obs_get_audio_info2(&audioInfo) && audioInfo.backend == OBS_AUDIO_BACKEND_ASIO)
		return true;

	return IsAsioProfileSelected(GetAsioProfileConfig());
}

static int GetAsioChannelCount(bool output)
{
	return output ? obs_asio_get_output_channel_count() : obs_asio_get_input_channel_count();
}

static const char *GetAsioChannelName(bool output, int index)
{
	return output ? obs_asio_get_output_channel_name(index) : obs_asio_get_input_channel_name(index);
}

static string MakeAsioRouteData(int left, int right)
{
	if (right < 0 || right == left)
		return std::to_string(left);

	return std::to_string(left) + "," + std::to_string(right);
}

static string MakeAsioDeviceSelection(int left, int right)
{
	return string(ASIO_DEVICE_PREFIX) + MakeAsioRouteData(left, right);
}

static const char *GetAsioRouteValue(const char *value)
{
	return IsASIOCaptureSelection(value) ? value + strlen(ASIO_DEVICE_PREFIX) : value;
}

static bool ParseAsioRouteData(const char *value, int &left, int &right)
{
	left = -1;
	right = -1;
	value = GetAsioRouteValue(value);

	if (!value || !*value)
		return false;

	string route(value);
	size_t commaPos = route.find(',');

	try {
		left = std::stoi(route.substr(0, commaPos));
		if (commaPos != string::npos)
			right = std::stoi(route.substr(commaPos + 1));
	} catch (...) {
		left = -1;
		right = -1;
		return false;
	}

	return left >= 0;
}

static void AddAsioRouteOptions(obs_property_t *prop, bool output)
{
	SyncAsioSelectedDeviceFromProfile();

	if (!IsASIOCaptureBackendActive()) {
		obs_property_list_add_string(prop, obs_module_text("AsioBackendRequired"), "");
		return;
	}

	if (GetAsioChannelCount(output) <= 0) {
		config_t *profile = GetAsioProfileConfig();
		const char *device = profile ? config_get_string(profile, "Audio", "ASIODevice") : nullptr;

		if (!device || !*device) {
			obs_property_list_add_string(prop, obs_module_text("AsioDeviceRequired"), "");
			return;
		}
	}

	int count = GetAsioChannelCount(output);
	if (count <= 0) {
		obs_property_list_add_string(prop, obs_module_text("AsioNoChannels"), "");
		return;
	}

	for (int i = 0; i < count; ++i) {
		string label = string("ASIO: ") + obs_module_text("AsioMono") + " " + std::to_string(i + 1);
		string value = MakeAsioRouteData(i, -1);
		obs_property_list_add_string(prop, label.c_str(), value.c_str());
	}

	for (int i = 0; i + 1 < count; i += 2) {
		string label = string("ASIO: ") + obs_module_text("AsioStereo") + " " +
			       std::to_string(i + 1) + "/" + std::to_string(i + 2);
		string value = MakeAsioRouteData(i, i + 1);
		obs_property_list_add_string(prop, label.c_str(), value.c_str());
	}
}

static void AddAsioDeviceOptions(obs_property_t *prop, bool output)
{
	SyncAsioSelectedDeviceFromProfile();

	if (!IsASIOCaptureBackendActive())
		return;

	if (GetAsioChannelCount(output) <= 0) {
		config_t *profile = GetAsioProfileConfig();
		const char *device = profile ? config_get_string(profile, "Audio", "ASIODevice") : nullptr;

		if (!device || !*device)
			return;
	}

	int count = GetAsioChannelCount(output);
	if (count <= 0)
		return;

	for (int i = 0; i < count; ++i) {
		string label = string("ASIO: ") + obs_module_text("AsioMono") + " " + std::to_string(i + 1);
		string value = MakeAsioDeviceSelection(i, -1);
		obs_property_list_add_string(prop, label.c_str(), value.c_str());
	}

	for (int i = 0; i + 1 < count; i += 2) {
		string label = string("ASIO: ") + obs_module_text("AsioStereo") + " " +
			       std::to_string(i + 1) + "/" + std::to_string(i + 2);
		string value = MakeAsioDeviceSelection(i, i + 1);
		obs_property_list_add_string(prop, label.c_str(), value.c_str());
	}
}

void AddASIOInputCaptureOptions(obs_property_t *prop)
{
	AddAsioDeviceOptions(prop, false);
}

void AddASIOOutputCaptureOptions(obs_property_t *prop)
{
	AddAsioDeviceOptions(prop, true);
}

class ASIOCaptureSource {
public:
	ASIOCaptureSource(obs_data_t *settings, obs_source_t *source_, bool output_)
		: source(source_), output(output_)
	{
		tap = obs_asio_bridge_create_tap(output, AudioCallback, this);
		Update(settings);
	}

	~ASIOCaptureSource()
	{
		obs_asio_bridge_destroy_tap(tap);
	}

	void Update(obs_data_t *settings)
	{
		int left;
		int right;
		const char *routeValue = obs_data_get_string(settings, OPT_CHANNEL_ID);
		if (!routeValue || !*routeValue)
			routeValue = obs_data_get_string(settings, OPT_DEVICE_ID);

		if (!ParseAsioRouteData(routeValue, left, right)) {
			left = -1;
			right = -1;
		}

		obs_asio_bridge_tap_set_channels(tap, left, right);
	}

	void Activate()
	{
		active = true;
	}

	void Deactivate()
	{
		active = false;
	}

private:
	static void AudioCallback(const float *left, const float *right, int num_frames, uint32_t sample_rate,
				  uint64_t timestamp, void *user_data)
	{
		static_cast<ASIOCaptureSource *>(user_data)->OnAudio(left, right, num_frames, sample_rate,
							 timestamp);
	}

	void OnAudio(const float *left, const float *right, int num_frames, uint32_t sample_rate,
		     uint64_t timestamp)
	{
		if (!active.load() || !source || !left || num_frames <= 0 || sample_rate == 0)
			return;

		obs_source_audio audio = {};
		audio.data[0] = reinterpret_cast<const uint8_t *>(left);
		audio.frames = (uint32_t)num_frames;
		audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
		audio.samples_per_sec = sample_rate;
		audio.timestamp = timestamp;

		if (right) {
			audio.data[1] = reinterpret_cast<const uint8_t *>(right);
			audio.speakers = SPEAKERS_STEREO;
		} else {
			audio.speakers = SPEAKERS_MONO;
		}

		obs_source_output_audio(source, &audio);
	}

	obs_source_t *source = nullptr;
	struct obs_asio_bridge_tap *tap = nullptr;
	bool output = false;
	std::atomic_bool active = false;
};

static const char *GetASIOInputName(void *)
{
	return obs_module_text("AsioInput");
}

static const char *GetASIOOutputName(void *)
{
	return obs_module_text("AsioOutput");
}

void GetASIOCaptureDefaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, OPT_CHANNEL_ID, "0,1");
}

void *CreateASIOInputCaptureSource(obs_data_t *settings, obs_source_t *source)
{
	return new ASIOCaptureSource(settings, source, false);
}

void *CreateASIOOutputCaptureSource(obs_data_t *settings, obs_source_t *source)
{
	return new ASIOCaptureSource(settings, source, true);
}

void DestroyASIOCaptureSource(void *data)
{
	delete static_cast<ASIOCaptureSource *>(data);
}

void UpdateASIOCaptureSource(void *data, obs_data_t *settings)
{
	static_cast<ASIOCaptureSource *>(data)->Update(settings);
}

void ActivateASIOCaptureSource(void *data)
{
	static_cast<ASIOCaptureSource *>(data)->Activate();
}

void DeactivateASIOCaptureSource(void *data)
{
	static_cast<ASIOCaptureSource *>(data)->Deactivate();
}

static obs_properties_t *GetASIOProperties(bool output)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *channelProp = obs_properties_add_list(props, OPT_CHANNEL_ID,
						      obs_module_text("AsioChannel"),
						      OBS_COMBO_TYPE_LIST,
						      OBS_COMBO_FORMAT_STRING);

	AddAsioRouteOptions(channelProp, output);
	return props;
}

obs_properties_t *GetASIOInputCaptureProperties(void *)
{
	return GetASIOProperties(false);
}

obs_properties_t *GetASIOOutputCaptureProperties(void *)
{
	return GetASIOProperties(true);
}

void RegisterASIOInput()
{
	obs_source_info info = {};
	info.id = "asio_input_capture";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.get_name = GetASIOInputName;
	info.create = CreateASIOInputCaptureSource;
	info.destroy = DestroyASIOCaptureSource;
	info.update = UpdateASIOCaptureSource;
	info.activate = ActivateASIOCaptureSource;
	info.deactivate = DeactivateASIOCaptureSource;
	info.get_defaults = GetASIOCaptureDefaults;
	info.get_properties = GetASIOInputCaptureProperties;
	info.icon_type = OBS_ICON_TYPE_AUDIO_INPUT;
	obs_register_source(&info);
}

void RegisterASIOOutput()
{
	obs_source_info info = {};
	info.id = "asio_output_capture";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_DO_NOT_SELF_MONITOR;
	info.get_name = GetASIOOutputName;
	info.create = CreateASIOOutputCaptureSource;
	info.destroy = DestroyASIOCaptureSource;
	info.update = UpdateASIOCaptureSource;
	info.activate = ActivateASIOCaptureSource;
	info.deactivate = DeactivateASIOCaptureSource;
	info.get_defaults = GetASIOCaptureDefaults;
	info.get_properties = GetASIOOutputCaptureProperties;
	info.icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT;
	obs_register_source(&info);
}