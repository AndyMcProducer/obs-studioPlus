// ASIO-backed implementation (Windows)
#ifdef _WIN32

#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <cstring>

#include "obs-asio.h"

// ASIO SDK interface
#include "iasiodrv.h"

static std::vector<std::string> g_device_names;
static std::vector<std::string> g_device_clsids;
static std::vector<std::string> g_device_keynames;
static std::mutex g_asio_mutex;
static std::string g_selected_device_name;
// The Steinberg host helpers expect this global driver pointer to exist.
IASIO *theAsioDriver = nullptr;
static IASIO *&g_current_asio = theAsioDriver;
static int g_current_driver_index = -1;

// ASIO runtime state for streaming
static ASIOBufferInfo *g_buffer_infos = nullptr;
static long g_input_buffer_num_channels = 0;
static long g_output_buffer_num_channels = 0;
static long g_total_buffer_count = 0;
static long g_buffer_size = 0;
static ASIOCallbacks g_asio_callbacks = {0};
static std::vector<ASIOSampleType> g_input_channel_types;
static std::vector<ASIOSampleType> g_output_channel_types;
static std::vector<std::vector<float>> g_input_channel_float_buffers;
static std::vector<std::vector<float>> g_output_channel_float_buffers;
static std::vector<std::string> g_input_channel_names;
static std::vector<std::string> g_output_channel_names;
static std::string g_channel_cache_device_name;
static struct obs_asio_context *g_ctx = nullptr;
static bool g_time_info_mode = false;

// Helper: byte-swap 32-bit
static inline uint32_t swap32(uint32_t v)
{
    return ((v & 0x000000FFU) << 24) | ((v & 0x0000FF00U) << 8) | ((v & 0x00FF0000U) >> 8) | ((v & 0xFF000000U) >> 24);
}

static inline float clamp_sample(float sample)
{
    if (sample > 1.0f)
        return 1.0f;
    if (sample < -1.0f)
        return -1.0f;
    return sample;
}

static inline size_t asio_sample_size_bytes(ASIOSampleType type)
{
    switch (type) {
    case ASIOSTFloat64LSB:
        return sizeof(double);
    case ASIOSTInt24LSB:
        return 3;
    case ASIOSTFloat32LSB:
    case ASIOSTFloat32MSB:
    case ASIOSTInt32LSB:
        return sizeof(float);
    case ASIOSTInt16LSB:
    case ASIOSTInt16MSB:
        return sizeof(int16_t);
    default:
        return sizeof(float);
    }
}

// ASIO callback implementations
static void convert_channel_to_float(ASIOSampleType type, void *src, float *dst, long frames);
static void convert_float_to_channel(ASIOSampleType type, const float *src, void *dst, long frames);
static void asio_buffer_switch(long index, ASIOBool directProcess)
{
    (void)directProcess;
    if (!g_ctx || !g_buffer_infos)
        return;

    if (g_ctx->audio_cb) {
        for (long ch = 0; ch < g_input_buffer_num_channels; ++ch) {
            void *src = g_buffer_infos[ch].buffers[index];
            float *dst = g_input_channel_float_buffers[(size_t)ch].data();
            convert_channel_to_float(g_input_channel_types[(size_t)ch], src, dst, g_buffer_size);
        }

        static std::vector<float *> input_ptrs;
        input_ptrs.resize((size_t)g_input_buffer_num_channels);
        for (long ch = 0; ch < g_input_buffer_num_channels; ++ch)
            input_ptrs[(size_t)ch] = g_input_channel_float_buffers[(size_t)ch].data();

        g_ctx->audio_cb(input_ptrs.data(), (int)g_input_buffer_num_channels, (int)g_buffer_size,
		       g_ctx->audio_cb_data);
    }

    if (g_output_buffer_num_channels <= 0)
        return;

    static std::vector<float *> output_ptrs;
    output_ptrs.resize((size_t)g_output_buffer_num_channels);
    for (long ch = 0; ch < g_output_buffer_num_channels; ++ch) {
        std::vector<float> &buffer = g_output_channel_float_buffers[(size_t)ch];
        memset(buffer.data(), 0, sizeof(float) * (size_t)g_buffer_size);
        output_ptrs[(size_t)ch] = buffer.data();
    }

    if (g_ctx->output_cb) {
        g_ctx->output_cb(output_ptrs.data(), (int)g_output_buffer_num_channels, (int)g_buffer_size,
			 g_ctx->output_cb_data);
    }

    const long output_offset = g_input_buffer_num_channels;
    for (long ch = 0; ch < g_output_buffer_num_channels; ++ch) {
        void *dst = g_buffer_infos[output_offset + ch].buffers[index];
        convert_float_to_channel(g_output_channel_types[(size_t)ch],
				 output_ptrs[(size_t)ch], dst, g_buffer_size);
    }
}

static ASIOTime *asio_buffer_switch_timeinfo(ASIOTime *params, long index, ASIOBool directProcess)
{
    (void)params;
    asio_buffer_switch(index, directProcess);
    return params;
}

static long asio_message(long selector, long value, void *message, double *opt)
{
    (void)value; (void)message; (void)opt;
    switch (selector) {
    case kAsioSelectorSupported:
        return 1; // we support selectors asked by drivers
    case kAsioEngineVersion:
        return 2; // host implementation version
    case kAsioResetRequest:
        return 1;
    case kAsioSupportsTimeInfo:
        g_time_info_mode = true;
        return 1;
    default:
        return 0;
    }
}

// Convert a single channel buffer to float samples
static void convert_channel_to_float(ASIOSampleType type, void *src, float *dst, long frames)
{
    if (!src || !dst || frames <= 0)
        return;

    switch (type) {
    case ASIOSTFloat32LSB: {
        float *s = (float *)src;
        for (long i = 0; i < frames; ++i)
            dst[i] = s[i];
        break;
    }
    case ASIOSTFloat32MSB: {
        uint32_t *s = (uint32_t *)src;
        for (long i = 0; i < frames; ++i) {
            uint32_t v = swap32(s[i]);
            float f; memcpy(&f, &v, sizeof(float));
            dst[i] = f;
        }
        break;
    }
    case ASIOSTInt16LSB: {
        int16_t *s = (int16_t *)src;
        for (long i = 0; i < frames; ++i)
            dst[i] = (float)s[i] / 32768.0f;
        break;
    }
    case ASIOSTInt16MSB: {
        unsigned char *b = (unsigned char *)src;
        for (long i = 0; i < frames; ++i) {
            int16_t v = (int16_t)((b[0] << 8) | b[1]);
            dst[i] = (float)v / 32768.0f;
            b += 2;
        }
        break;
    }
    case ASIOSTInt24LSB: {
        unsigned char *b = (unsigned char *)src;
        for (long i = 0; i < frames; ++i) {
            int32_t v = (int32_t)(b[0] | (b[1] << 8) | (b[2] << 16));
            if (v & 0x800000) v |= 0xFF000000; // sign extend
            dst[i] = (float)v / 8388608.0f;
            b += 3;
        }
        break;
    }
    case ASIOSTInt32LSB: {
        int32_t *s = (int32_t *)src;
        for (long i = 0; i < frames; ++i)
            dst[i] = (float)s[i] / 2147483648.0f;
        break;
    }
    case ASIOSTFloat64LSB: {
        double *s = (double *)src;
        for (long i = 0; i < frames; ++i)
            dst[i] = (float)s[i];
        break;
    }
    default:
        // Unsupported: zero out
        for (long i = 0; i < frames; ++i)
            dst[i] = 0.0f;
        break;
    }
}

static void convert_float_to_channel(ASIOSampleType type, const float *src, void *dst, long frames)
{
    if (!src || !dst || frames <= 0)
        return;

    switch (type) {
    case ASIOSTFloat32LSB: {
        float *d = (float *)dst;
        for (long i = 0; i < frames; ++i)
            d[i] = clamp_sample(src[i]);
        break;
    }
    case ASIOSTFloat32MSB: {
        uint32_t *d = (uint32_t *)dst;
        for (long i = 0; i < frames; ++i) {
            float sample = clamp_sample(src[i]);
            uint32_t bits = 0;
            memcpy(&bits, &sample, sizeof(bits));
            d[i] = swap32(bits);
        }
        break;
    }
    case ASIOSTInt16LSB: {
        int16_t *d = (int16_t *)dst;
        for (long i = 0; i < frames; ++i)
            d[i] = (int16_t)(clamp_sample(src[i]) * 32767.0f);
        break;
    }
    case ASIOSTInt16MSB: {
        unsigned char *d = (unsigned char *)dst;
        for (long i = 0; i < frames; ++i) {
            int16_t sample = (int16_t)(clamp_sample(src[i]) * 32767.0f);
            d[0] = (unsigned char)((sample >> 8) & 0xFF);
            d[1] = (unsigned char)(sample & 0xFF);
            d += 2;
        }
        break;
    }
    case ASIOSTInt24LSB: {
        unsigned char *d = (unsigned char *)dst;
        for (long i = 0; i < frames; ++i) {
            int32_t sample = (int32_t)(clamp_sample(src[i]) * 8388607.0f);
            d[0] = (unsigned char)(sample & 0xFF);
            d[1] = (unsigned char)((sample >> 8) & 0xFF);
            d[2] = (unsigned char)((sample >> 16) & 0xFF);
            d += 3;
        }
        break;
    }
    case ASIOSTInt32LSB: {
        int32_t *d = (int32_t *)dst;
        for (long i = 0; i < frames; ++i)
            d[i] = (int32_t)(clamp_sample(src[i]) * 2147483647.0f);
        break;
    }
    case ASIOSTFloat64LSB: {
        double *d = (double *)dst;
        for (long i = 0; i < frames; ++i)
            d[i] = (double)clamp_sample(src[i]);
        break;
    }
    default:
        memset(dst, 0, (size_t)frames * asio_sample_size_bytes(type));
        break;
    }
}

// ASIO callbacks
static void asio_buffer_switch(long index, ASIOBool directProcess);
static ASIOTime *asio_buffer_switch_timeinfo(ASIOTime *params, long index, ASIOBool directProcess);
static void asio_sample_rate_changed(ASIOSampleRate sRate) { (void)sRate; }
static long asio_message(long selector, long value, void *message, double *opt);

static void ensure_device_list()
{
    std::lock_guard<std::mutex> lock(g_asio_mutex);

    if (!g_device_names.empty())
        return;

    HKEY hkey = NULL;
    // Try standard registry path for ASIO drivers
    LONG cr = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", 0, KEY_READ, &hkey);
    if (cr != ERROR_SUCCESS) {
        // Try explicit 64/32-bit views if initial open failed
        cr = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", 0, KEY_READ | KEY_WOW64_64KEY, &hkey);
        if (cr != ERROR_SUCCESS)
            cr = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", 0, KEY_READ | KEY_WOW64_32KEY, &hkey);
    }

    if (cr != ERROR_SUCCESS || !hkey)
        return;

    char keyname[256];
    DWORD index = 0;
    while (true) {
        DWORD keylen = (DWORD)sizeof(keyname);
        FILETIME ft = {0};
        LONG rc = RegEnumKeyExA(hkey, index, keyname, &keylen, NULL, NULL, NULL, &ft);
        if (rc != ERROR_SUCCESS)
            break;

        HKEY hsub = NULL;
        if (RegOpenKeyExA(hkey, keyname, 0, KEY_READ, &hsub) == ERROR_SUCCESS) {
            char clsidbuf[256] = {0};
            DWORD clsidlen = (DWORD)sizeof(clsidbuf);
            DWORD type = 0;
            RegQueryValueExA(hsub, "clsid", NULL, &type, (LPBYTE)clsidbuf, &clsidlen);

            char descbuf[256] = {0};
            DWORD desclen = (DWORD)sizeof(descbuf);
            if (RegQueryValueExA(hsub, "description", NULL, &type, (LPBYTE)descbuf, &desclen) != ERROR_SUCCESS)
                descbuf[0] = '\0';

            std::string name = descbuf[0] ? descbuf : keyname;
            std::string clsid = clsidbuf[0] ? clsidbuf : std::string();

            g_device_names.push_back(name);
            g_device_clsids.push_back(clsid);
            g_device_keynames.push_back(keyname);

            RegCloseKey(hsub);
        }

        index++;
    }

    RegCloseKey(hkey);
}

static size_t find_selected_device_index()
{
    if (g_selected_device_name.empty())
        return SIZE_MAX;

    for (size_t i = 0; i < g_device_names.size(); ++i) {
        if (g_device_clsids[i].empty())
            continue;

        if (strcmp(g_device_names[i].c_str(), g_selected_device_name.c_str()) == 0)
            return i;

        if (strcmp(g_device_keynames[i].c_str(), g_selected_device_name.c_str()) == 0)
            return i;
    }

    return SIZE_MAX;
}

static size_t find_first_usable_device_index()
{
    for (size_t i = 0; i < g_device_clsids.size(); ++i) {
        if (!g_device_clsids[i].empty())
            return i;
    }

    return SIZE_MAX;
}

static bool create_driver_for_index(size_t index, IASIO **out_driver, bool *out_coinited)
{
    if (!out_driver || !out_coinited || index >= g_device_clsids.size())
        return false;

    const std::string &clsidstr = g_device_clsids[index];
    if (clsidstr.empty())
        return false;

    int len = MultiByteToWideChar(CP_ACP, 0, clsidstr.c_str(), -1, NULL, 0);
    if (len <= 0)
        return false;

    std::vector<wchar_t> wclsid((size_t)len);
    MultiByteToWideChar(CP_ACP, 0, clsidstr.c_str(), -1, wclsid.data(), len);

    CLSID clsid;
    if (CLSIDFromString((LPOLESTR)wclsid.data(), &clsid) != S_OK)
        return false;

    HRESULT co = CoInitialize(NULL);
    bool coinited = SUCCEEDED(co);

    IASIO *iasio = nullptr;
    HRESULT hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, clsid, (void **)&iasio);
    if (FAILED(hr) || !iasio) {
        if (coinited)
            CoUninitialize();
        return false;
    }

    ASIOBool ok = iasio->init(NULL);
    if (!ok) {
        iasio->Release();
        if (coinited)
            CoUninitialize();
        return false;
    }

    *out_driver = iasio;
    *out_coinited = coinited;
    return true;
}

static void release_temporary_driver(IASIO *iasio, bool coinited)
{
    if (!iasio)
        return;

    iasio->Release();
    if (coinited)
        CoUninitialize();
}

static void fill_channel_names(IASIO *iasio, std::vector<std::string> &inputs, std::vector<std::string> &outputs)
{
    long numInputs = 0;
    long numOutputs = 0;

    inputs.clear();
    outputs.clear();

    if (!iasio || iasio->getChannels(&numInputs, &numOutputs) != ASE_OK)
        return;

    inputs.reserve((size_t)(numInputs > 0 ? numInputs : 0));
    outputs.reserve((size_t)(numOutputs > 0 ? numOutputs : 0));

    for (long i = 0; i < numInputs; ++i) {
        ASIOChannelInfo info = {};
        info.channel = i;
        info.isInput = ASIOTrue;

        if (iasio->getChannelInfo(&info) == ASE_OK && info.name[0])
            inputs.emplace_back(info.name);
        else
            inputs.emplace_back("Input " + std::to_string((long long)i + 1));
    }

    for (long i = 0; i < numOutputs; ++i) {
        ASIOChannelInfo info = {};
        info.channel = i;
        info.isInput = ASIOFalse;

        if (iasio->getChannelInfo(&info) == ASE_OK && info.name[0])
            outputs.emplace_back(info.name);
        else
            outputs.emplace_back("Output " + std::to_string((long long)i + 1));
    }
}

static void refresh_channel_cache_locked()
{
    size_t selected = find_selected_device_index();
    if (selected == SIZE_MAX)
        selected = find_first_usable_device_index();

    if (selected == SIZE_MAX) {
        g_input_channel_names.clear();
        g_output_channel_names.clear();
        g_channel_cache_device_name.clear();
        return;
    }

    const std::string cache_name = g_device_names[selected];
    if (cache_name == g_channel_cache_device_name &&
	(!g_input_channel_names.empty() || !g_output_channel_names.empty())) {
        return;
    }

    IASIO *iasio = nullptr;
    bool release_driver = false;
    bool coinited = false;

    if (g_current_asio && (int)selected == g_current_driver_index) {
        iasio = g_current_asio;
    } else if (!create_driver_for_index(selected, &iasio, &coinited)) {
        g_input_channel_names.clear();
        g_output_channel_names.clear();
        g_channel_cache_device_name = cache_name;
        return;
    } else {
        release_driver = true;
    }

    fill_channel_names(iasio, g_input_channel_names, g_output_channel_names);
    g_channel_cache_device_name = cache_name;

    if (release_driver)
        release_temporary_driver(iasio, coinited);
}

extern "C" {

EXPORT int obs_asio_get_device_count(void)
{
    ensure_device_list();
    return (int)g_device_names.size();
}

EXPORT const char *obs_asio_get_device_name(int index)
{
    ensure_device_list();
    if (index < 0 || index >= (int)g_device_names.size())
        return nullptr;
    return g_device_names[(size_t)index].c_str();
}

EXPORT int obs_asio_get_input_channel_count(void)
{
    ensure_device_list();
    std::lock_guard<std::mutex> lock(g_asio_mutex);
    refresh_channel_cache_locked();
    return (int)g_input_channel_names.size();
}

EXPORT const char *obs_asio_get_input_channel_name(int index)
{
    ensure_device_list();
    std::lock_guard<std::mutex> lock(g_asio_mutex);
    refresh_channel_cache_locked();
    if (index < 0 || index >= (int)g_input_channel_names.size())
        return nullptr;
    return g_input_channel_names[(size_t)index].c_str();
}

EXPORT int obs_asio_get_output_channel_count(void)
{
    ensure_device_list();
    std::lock_guard<std::mutex> lock(g_asio_mutex);
    refresh_channel_cache_locked();
    return (int)g_output_channel_names.size();
}

EXPORT const char *obs_asio_get_output_channel_name(int index)
{
    ensure_device_list();
    std::lock_guard<std::mutex> lock(g_asio_mutex);
    refresh_channel_cache_locked();
    if (index < 0 || index >= (int)g_output_channel_names.size())
        return nullptr;
    return g_output_channel_names[(size_t)index].c_str();
}

EXPORT void obs_asio_set_selected_device(const char *device_name)
{
    std::lock_guard<std::mutex> lock(g_asio_mutex);
    g_selected_device_name = device_name ? device_name : "";
	g_channel_cache_device_name.clear();
	g_input_channel_names.clear();
	g_output_channel_names.clear();
}

EXPORT bool obs_asio_open_control_panel(const char *device_name)
{
    if (!device_name)
        return false;

    ensure_device_list();
    std::lock_guard<std::mutex> lock(g_asio_mutex);

    for (size_t i = 0; i < g_device_names.size(); ++i) {
        if (strcmp(g_device_names[i].c_str(), device_name) != 0 && strcmp(g_device_keynames[i].c_str(), device_name) != 0)
            continue;

        std::string clsidstr = g_device_clsids[i];
        if (clsidstr.empty()) {
            // Try to read CLSID from registry for this key
            std::string subkey = std::string("SOFTWARE\\ASIO\\") + g_device_keynames[i];
            HKEY hsub = NULL;
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, subkey.c_str(), 0, KEY_READ, &hsub) == ERROR_SUCCESS) {
                char clsidbuf[256] = {0};
                DWORD clsidlen = (DWORD)sizeof(clsidbuf);
                DWORD type = 0;
                if (RegQueryValueExA(hsub, "clsid", NULL, &type, (LPBYTE)clsidbuf, &clsidlen) == ERROR_SUCCESS)
                    clsidstr = clsidbuf;
                RegCloseKey(hsub);
            }
        }

        if (clsidstr.empty())
            continue;

        // Convert CLSID string to CLSID
        int len = MultiByteToWideChar(CP_ACP, 0, clsidstr.c_str(), -1, NULL, 0);
        if (len <= 0)
            continue;
        std::vector<wchar_t> wclsid(len);
        MultiByteToWideChar(CP_ACP, 0, clsidstr.c_str(), -1, wclsid.data(), len);

        CLSID clsid;
        if (CLSIDFromString((LPOLESTR)wclsid.data(), &clsid) != S_OK)
            continue;

        HRESULT co = CoInitialize(NULL);
        bool inited = SUCCEEDED(co);

        IASIO *iasio = nullptr;
        // Some ASIO drivers expect the class id to be used as the IID as well (historical SDK pattern)
        HRESULT hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, clsid, (void **)&iasio);
        if (FAILED(hr) || !iasio) {
            if (inited)
                CoUninitialize();
            continue;
        }

        ASIOError err = iasio->controlPanel();
        iasio->Release();

        if (inited)
            CoUninitialize();

        return (err == 0);
    }

    return false;
}

EXPORT bool obs_asio_initialize(struct obs_asio_context *ctx, long sample_rate, long buffer_size, int num_channels)
{
    if (!ctx)
        return false;

    ensure_device_list();
    std::lock_guard<std::mutex> lock(g_asio_mutex);

    if (!g_current_asio) {
        size_t found = find_selected_device_index();

        if (found == SIZE_MAX)
            found = find_first_usable_device_index();

        if (found == SIZE_MAX)
            return false;

        bool coinited = false;
        IASIO *iasio = nullptr;
        if (!create_driver_for_index(found, &iasio, &coinited))
            return false;

        g_current_asio = iasio;
        g_current_driver_index = (int)found;
    }

    // Query available channels
    long numInputs = 0, numOutputs = 0;
    if (g_current_asio->getChannels(&numInputs, &numOutputs) != ASE_OK)
        return false;
    if (numInputs <= 0)
        return false;

    refresh_channel_cache_locked();

    // Handle buffer size: if 0, query preferred from driver
    long minSize = 0, maxSize = 0, preferredSize = 0, granularity = 0;
    if (g_current_asio->getBufferSize(&minSize, &maxSize, &preferredSize, &granularity) == ASE_OK) {
        if (buffer_size <= 0)
            buffer_size = preferredSize;
        else if (buffer_size < minSize)
            buffer_size = minSize;
        else if (buffer_size > maxSize)
            buffer_size = maxSize;
    } else if (buffer_size <= 0) {
        buffer_size = 512; // Fallback
    }

    long use_channels = num_channels > 0 ? num_channels : numInputs;
    if (use_channels > numInputs)
        use_channels = numInputs;

    // Prepare buffers and callbacks
    g_input_buffer_num_channels = use_channels;
    g_output_buffer_num_channels = numOutputs > 0 ? numOutputs : 0;
    g_total_buffer_count = g_input_buffer_num_channels + g_output_buffer_num_channels;
    g_buffer_size = buffer_size;

    // Allocate buffer info array
    if (g_buffer_infos)
        delete[] g_buffer_infos;
    g_buffer_infos = new ASIOBufferInfo[(size_t)g_total_buffer_count];
    memset(g_buffer_infos, 0, sizeof(ASIOBufferInfo) * (size_t)g_total_buffer_count);

    for (long i = 0; i < g_input_buffer_num_channels; ++i) {
        g_buffer_infos[i].isInput = ASIOTrue;
        g_buffer_infos[i].channelNum = (long)i;
        g_buffer_infos[i].buffers[0] = g_buffer_infos[i].buffers[1] = NULL;
    }

    for (long i = 0; i < g_output_buffer_num_channels; ++i) {
        const long idx = g_input_buffer_num_channels + i;
        g_buffer_infos[idx].isInput = ASIOFalse;
        g_buffer_infos[idx].channelNum = (long)i;
        g_buffer_infos[idx].buffers[0] = g_buffer_infos[idx].buffers[1] = NULL;
    }

    // Setup callbacks
    memset(&g_asio_callbacks, 0, sizeof(g_asio_callbacks));
    g_asio_callbacks.bufferSwitch = asio_buffer_switch;
    g_asio_callbacks.sampleRateDidChange = asio_sample_rate_changed;
    g_asio_callbacks.asioMessage = asio_message;
    g_asio_callbacks.bufferSwitchTimeInfo = asio_buffer_switch_timeinfo;

    ASIOError err = g_current_asio->createBuffers(g_buffer_infos, g_total_buffer_count, g_buffer_size,
					 &g_asio_callbacks);
    if (err != ASE_OK)
        return false;

    // Determine input channel sample types and allocate float buffers
    g_input_channel_types.resize((size_t)g_input_buffer_num_channels);
    g_input_channel_float_buffers.clear();
    g_input_channel_float_buffers.resize((size_t)g_input_buffer_num_channels);
    for (long i = 0; i < g_input_buffer_num_channels; ++i) {
        ASIOChannelInfo info;
        memset(&info, 0, sizeof(info));
        info.channel = (long)i;
        info.isInput = ASIOTrue;
        if (g_current_asio->getChannelInfo(&info) == ASE_OK)
            g_input_channel_types[(size_t)i] = info.type;
        else
            g_input_channel_types[(size_t)i] = ASIOSTFloat32LSB;

        g_input_channel_float_buffers[(size_t)i].assign((size_t)g_buffer_size, 0.0f);
    }

    g_output_channel_types.resize((size_t)g_output_buffer_num_channels);
    g_output_channel_float_buffers.clear();
    g_output_channel_float_buffers.resize((size_t)g_output_buffer_num_channels);
    for (long i = 0; i < g_output_buffer_num_channels; ++i) {
        ASIOChannelInfo info;
        memset(&info, 0, sizeof(info));
        info.channel = (long)i;
        info.isInput = ASIOFalse;
        if (g_current_asio->getChannelInfo(&info) == ASE_OK)
            g_output_channel_types[(size_t)i] = info.type;
        else
            g_output_channel_types[(size_t)i] = ASIOSTFloat32LSB;

        g_output_channel_float_buffers[(size_t)i].assign((size_t)g_buffer_size, 0.0f);
    }

    // Save context for callbacks
    g_ctx = ctx;

    ctx->driver = g_current_asio;
    ctx->sample_rate = sample_rate;
    ctx->buffer_size = buffer_size;
    ctx->num_channels = (int)g_input_buffer_num_channels;
    ctx->num_input_channels = (int)g_input_buffer_num_channels;
    ctx->num_output_channels = (int)g_output_buffer_num_channels;

    return true;
}

EXPORT void obs_asio_set_audio_callback(struct obs_asio_context *ctx, obs_asio_audio_callback_t cb, void *user_data)
{
    if (!ctx)
        return;
    ctx->audio_cb = cb;
    ctx->audio_cb_data = user_data;
}

EXPORT void obs_asio_set_output_callback(struct obs_asio_context *ctx, obs_asio_audio_output_callback_t cb,
					 void *user_data)
{
    if (!ctx)
        return;
    ctx->output_cb = cb;
    ctx->output_cb_data = user_data;
}

EXPORT void obs_asio_shutdown(struct obs_asio_context *ctx)
{
    std::lock_guard<std::mutex> lock(g_asio_mutex);
    if (ctx && ctx->driver) {
        IASIO *iasio = (IASIO *)ctx->driver;
        // Stop and dispose buffers
        iasio->stop();
        iasio->disposeBuffers();

        // Clean up host-side allocations
        if (g_buffer_infos) {
            delete[] g_buffer_infos;
            g_buffer_infos = nullptr;
        }
        g_input_channel_float_buffers.clear();
        g_output_channel_float_buffers.clear();
        g_input_channel_types.clear();
        g_output_channel_types.clear();
        g_input_buffer_num_channels = 0;
        g_output_buffer_num_channels = 0;
        g_total_buffer_count = 0;
        g_buffer_size = 0;

        iasio->Release();
        ctx->driver = nullptr;
        g_current_asio = nullptr;
        g_current_driver_index = -1;
        g_ctx = nullptr;
        CoUninitialize();
    }
}

EXPORT bool obs_asio_start(struct obs_asio_context *ctx)
{
    if (!ctx || !ctx->driver)
        return false;
    IASIO *iasio = (IASIO *)ctx->driver;
    ASIOError err = iasio->start();
    return (err == 0);
}

EXPORT void obs_asio_stop(struct obs_asio_context *ctx)
{
    if (!ctx || !ctx->driver)
        return;
    IASIO *iasio = (IASIO *)ctx->driver;
    iasio->stop();
}

} // extern "C"

#endif // _WIN32
