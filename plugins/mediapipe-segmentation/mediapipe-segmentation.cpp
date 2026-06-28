#include <obs-module.h>
#include <obs-source.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/dstr.h>
#include <util/platform.h>

#include "runtime/mediapipe-runtime.h"

#include <string>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("mediapipe-segmentation", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "MediaPipe selfie segmentation filter";
}

#define do_log(level, format, ...) \
	blog(level, "[MediaPipe Segmentation: '%s'] " format, obs_source_get_name(filter->context), ##__VA_ARGS__)
#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define error(format, ...) do_log(LOG_ERROR, format, ##__VA_ARGS__)

#define S_RUNTIME_PATH "runtime_path"
#define S_MODEL_PATH "model_path"
#define S_OUTPUT_MODE "output_mode"
#define S_BACKGROUND_COLOR "background_color"
#define S_THRESHOLD "threshold"
#define S_SOFTNESS "softness"
#define S_BLUR_RADIUS "blur_radius"
#define S_PROCESSING_INTERVAL "processing_interval"

#define T_(text) obs_module_text(text)
#define T_RUNTIME_WARNING T_("MediaPipeSegmentation.RuntimeWarning")
#define T_RUNTIME_PATH T_("MediaPipeSegmentation.RuntimePath")
#define T_MODEL_PATH T_("MediaPipeSegmentation.ModelPath")
#define T_OUTPUT_MODE T_("MediaPipeSegmentation.OutputMode")
#define T_OUTPUT_TRANSPARENT T_("MediaPipeSegmentation.OutputMode.Transparent")
#define T_OUTPUT_GREEN T_("MediaPipeSegmentation.OutputMode.Green")
#define T_OUTPUT_BLUR T_("MediaPipeSegmentation.OutputMode.Blur")
#define T_OUTPUT_MASK T_("MediaPipeSegmentation.OutputMode.Mask")
#define T_BACKGROUND_COLOR T_("MediaPipeSegmentation.BackgroundColor")
#define T_THRESHOLD T_("MediaPipeSegmentation.Threshold")
#define T_SOFTNESS T_("MediaPipeSegmentation.Softness")
#define T_BLUR_RADIUS T_("MediaPipeSegmentation.BlurRadius")
#define T_PROCESSING_INTERVAL T_("MediaPipeSegmentation.ProcessingInterval")
#define T_PROCESSING_INTERVAL_HINT T_("MediaPipeSegmentation.ProcessingInterval.Hint")
#define T_BROWSE_MODELS T_("MediaPipeSegmentation.BrowsePath.Models")
#define T_BROWSE_LIBRARIES T_("MediaPipeSegmentation.BrowsePath.Libraries")
#define T_BROWSE_ALL_FILES T_("MediaPipeSegmentation.BrowsePath.AllFiles")

enum output_mode {
	OUTPUT_TRANSPARENT,
	OUTPUT_GREEN_SCREEN,
	OUTPUT_BLUR,
	OUTPUT_MASK_PREVIEW,
};

struct runtime_loader {
	void *module = nullptr;
	obs_mediapipe_runtime_create_t create = nullptr;
	obs_mediapipe_runtime_destroy_t destroy = nullptr;
	obs_mediapipe_runtime_process_rgba_t processRgba = nullptr;
	obs_mediapipe_runtime_version_t version = nullptr;
	obs_mediapipe_runtime_t instance = nullptr;
	uint32_t width = 0;
	uint32_t height = 0;
	std::string libraryPath;
	std::string modelPath;
	std::string lastError;
	bool loadAttempted = false;
	bool instanceCreateAttempted = false;
};

struct segmentation_filter {
	obs_source_t *context = nullptr;

	gs_effect_t *effect = nullptr;
	gs_eparam_t *imageParam = nullptr;
	gs_eparam_t *maskParam = nullptr;
	gs_eparam_t *backgroundColorParam = nullptr;
	gs_eparam_t *pixelSizeParam = nullptr;
	gs_eparam_t *thresholdParam = nullptr;
	gs_eparam_t *softnessParam = nullptr;
	gs_eparam_t *blurRadiusParam = nullptr;

	gs_texrender_t *render = nullptr;
	gs_stagesurf_t *stage = nullptr;
	gs_texture_t *maskTexture = nullptr;

	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<uint8_t> mask;
	bool maskReady = false;

	runtime_loader runtime;
	std::string runtimePathSetting;
	std::string modelPathSetting;

	output_mode mode = OUTPUT_TRANSPARENT;
	struct vec4 backgroundColor = {};
	float threshold = 0.50f;
	float softness = 0.08f;
	float blurRadius = 8.0f;
	int processingInterval = 1;
	int processingCounter = 0;
};

static std::string module_file_path(const char *file)
{
	char *path = obs_module_file(file);
	std::string result = path ? path : "";
	bfree(path);
	return result;
}

static bool empty_string(const std::string &value)
{
	return value.empty() || value[0] == '\0';
}

static std::string resolve_module_file(const std::string &setting, const char *fallback)
{
	if (!empty_string(setting))
		return setting;

	return module_file_path(fallback);
}

static void runtime_destroy_instance(runtime_loader &runtime)
{
	if (runtime.instance && runtime.destroy)
		runtime.destroy(runtime.instance);

	runtime.instance = nullptr;
	runtime.width = 0;
	runtime.height = 0;
	runtime.instanceCreateAttempted = false;
}

static void runtime_unload(runtime_loader &runtime)
{
	runtime_destroy_instance(runtime);

	if (runtime.module)
		os_dlclose(runtime.module);

	runtime.module = nullptr;
	runtime.create = nullptr;
	runtime.destroy = nullptr;
	runtime.processRgba = nullptr;
	runtime.version = nullptr;
	runtime.loadAttempted = false;
}

static bool runtime_load_library(struct segmentation_filter *filter)
{
	runtime_loader &runtime = filter->runtime;

	if (runtime.module)
		return true;

	if (runtime.loadAttempted)
		return false;

	runtime.loadAttempted = true;
	runtime.lastError.clear();

	if (runtime.libraryPath.empty()) {
		runtime.lastError = "No runtime DLL path configured";
		return false;
	}

	runtime.module = os_dlopen(runtime.libraryPath.c_str());
	if (!runtime.module) {
		runtime.lastError = "Could not load runtime DLL";
		return false;
	}

	runtime.create = (obs_mediapipe_runtime_create_t)os_dlsym(runtime.module, "obs_mediapipe_runtime_create");
	runtime.destroy = (obs_mediapipe_runtime_destroy_t)os_dlsym(runtime.module, "obs_mediapipe_runtime_destroy");
	runtime.processRgba =
		(obs_mediapipe_runtime_process_rgba_t)os_dlsym(runtime.module, "obs_mediapipe_runtime_process_rgba");
	runtime.version = (obs_mediapipe_runtime_version_t)os_dlsym(runtime.module, "obs_mediapipe_runtime_version");

	if (!runtime.create || !runtime.destroy || !runtime.processRgba) {
		runtime.lastError = "Runtime DLL is missing required MediaPipe segmentation exports";
		runtime_unload(runtime);
		return false;
	}

	if (runtime.version)
		blog(LOG_INFO, "[MediaPipe Segmentation] Runtime ABI version: %u", runtime.version());

	return true;
}

static bool runtime_ensure_instance(struct segmentation_filter *filter, uint32_t width, uint32_t height)
{
	runtime_loader &runtime = filter->runtime;

	if (!runtime_load_library(filter))
		return false;

	if (runtime.instance && runtime.width == width && runtime.height == height)
		return true;

	if (!runtime.instance && runtime.instanceCreateAttempted)
		return false;

	runtime_destroy_instance(runtime);
	runtime.instanceCreateAttempted = true;

	char errorBuffer[512] = {};
	runtime.instance = runtime.create(runtime.modelPath.c_str(), width, height, errorBuffer, sizeof(errorBuffer));
	if (!runtime.instance) {
		runtime.lastError = errorBuffer[0] ? errorBuffer : "Runtime failed to create segmentation instance";
		error("%s", runtime.lastError.c_str());
		return false;
	}

	runtime.width = width;
	runtime.height = height;
	return true;
}

static void destroy_graphics_resources(struct segmentation_filter *filter)
{
	obs_enter_graphics();

	gs_texture_destroy(filter->maskTexture);
	gs_stagesurface_destroy(filter->stage);
	gs_texrender_destroy(filter->render);

	filter->maskTexture = nullptr;
	filter->stage = nullptr;
	filter->render = nullptr;

	obs_leave_graphics();
}

static bool ensure_graphics_resources(struct segmentation_filter *filter, uint32_t width, uint32_t height)
{
	if (filter->width == width && filter->height == height && filter->render && filter->stage && filter->maskTexture)
		return true;

	destroy_graphics_resources(filter);

	filter->width = width;
	filter->height = height;
	filter->mask.assign((size_t)width * (size_t)height, 0);
	filter->maskReady = false;

	filter->render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	filter->stage = gs_stagesurface_create(width, height, GS_RGBA);
	filter->maskTexture = gs_texture_create(width, height, GS_R8, 1, nullptr, GS_DYNAMIC);

	if (!filter->render || !filter->stage || !filter->maskTexture) {
		error("Failed to allocate segmentation graphics resources");
		destroy_graphics_resources(filter);
		return false;
	}

	return true;
}

static const char *filter_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_("MediaPipeSegmentation");
}

static void filter_update(void *data, obs_data_t *settings)
{
	struct segmentation_filter *filter = (struct segmentation_filter *)data;

	std::string runtimePath = obs_data_get_string(settings, S_RUNTIME_PATH);
	std::string modelPath = obs_data_get_string(settings, S_MODEL_PATH);

	if (runtimePath != filter->runtimePathSetting || modelPath != filter->modelPathSetting) {
		filter->runtimePathSetting = runtimePath;
		filter->modelPathSetting = modelPath;
		runtime_unload(filter->runtime);
		filter->runtime.libraryPath = resolve_module_file(runtimePath, "obs-mediapipe-runtime.dll");
		filter->runtime.modelPath = resolve_module_file(modelPath, "selfie_segmenter.onnx");
	}

	filter->mode = (output_mode)obs_data_get_int(settings, S_OUTPUT_MODE);
	filter->threshold = (float)obs_data_get_double(settings, S_THRESHOLD);
	filter->softness = (float)obs_data_get_double(settings, S_SOFTNESS);
	filter->blurRadius = (float)obs_data_get_double(settings, S_BLUR_RADIUS);
	filter->processingInterval = (int)obs_data_get_int(settings, S_PROCESSING_INTERVAL);
	if (filter->processingInterval < 1)
		filter->processingInterval = 1;

	const uint32_t color = (uint32_t)obs_data_get_int(settings, S_BACKGROUND_COLOR);
	vec4_from_rgba(&filter->backgroundColor, color | 0xFF000000);
}

static void filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_OUTPUT_MODE, OUTPUT_TRANSPARENT);
	obs_data_set_default_int(settings, S_BACKGROUND_COLOR, 0x00FF00);
	obs_data_set_default_double(settings, S_THRESHOLD, 0.50);
	obs_data_set_default_double(settings, S_SOFTNESS, 0.08);
	obs_data_set_default_double(settings, S_BLUR_RADIUS, 8.0);
	obs_data_set_default_int(settings, S_PROCESSING_INTERVAL, 1);
}

static bool output_mode_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	const int mode = (int)obs_data_get_int(settings, S_OUTPUT_MODE);

	obs_property_set_visible(obs_properties_get(props, S_BACKGROUND_COLOR), mode == OUTPUT_GREEN_SCREEN);
	obs_property_set_visible(obs_properties_get(props, S_BLUR_RADIUS), mode == OUTPUT_BLUR);

	UNUSED_PARAMETER(property);
	return true;
}

static bool runtime_warning_visible(struct segmentation_filter *filter)
{
	return !filter || (!filter->runtime.module && !filter->runtime.instance);
}

static obs_properties_t *filter_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	struct dstr modelFilter = {};
	struct dstr libraryFilter = {};

	dstr_copy(&modelFilter, T_BROWSE_MODELS);
	dstr_cat(&modelFilter, " (*.tflite *.task);;");
	dstr_cat(&modelFilter, T_BROWSE_ALL_FILES);
	dstr_cat(&modelFilter, " (*.*)");

	dstr_copy(&libraryFilter, T_BROWSE_LIBRARIES);
	dstr_cat(&libraryFilter, " (*.dll);;");
	dstr_cat(&libraryFilter, T_BROWSE_ALL_FILES);
	dstr_cat(&libraryFilter, " (*.*)");

	obs_property_t *warning = obs_properties_add_text(props, "runtime_warning", T_RUNTIME_WARNING, OBS_TEXT_INFO);
	obs_property_text_set_info_type(warning, OBS_TEXT_INFO_WARNING);

	obs_properties_add_path(props, S_RUNTIME_PATH, T_RUNTIME_PATH, OBS_PATH_FILE, libraryFilter.array, nullptr);
	obs_properties_add_path(props, S_MODEL_PATH, T_MODEL_PATH, OBS_PATH_FILE, modelFilter.array, nullptr);

	obs_property_t *mode = obs_properties_add_list(props, S_OUTPUT_MODE, T_OUTPUT_MODE, OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, T_OUTPUT_TRANSPARENT, OUTPUT_TRANSPARENT);
	obs_property_list_add_int(mode, T_OUTPUT_GREEN, OUTPUT_GREEN_SCREEN);
	obs_property_list_add_int(mode, T_OUTPUT_BLUR, OUTPUT_BLUR);
	obs_property_list_add_int(mode, T_OUTPUT_MASK, OUTPUT_MASK_PREVIEW);
	obs_property_set_modified_callback(mode, output_mode_modified);

	obs_properties_add_color(props, S_BACKGROUND_COLOR, T_BACKGROUND_COLOR);
	obs_properties_add_float_slider(props, S_THRESHOLD, T_THRESHOLD, 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(props, S_SOFTNESS, T_SOFTNESS, 0.0, 0.5, 0.01);
	obs_properties_add_float_slider(props, S_BLUR_RADIUS, T_BLUR_RADIUS, 1.0, 32.0, 1.0);
	obs_property_t *interval =
		obs_properties_add_int_slider(props, S_PROCESSING_INTERVAL, T_PROCESSING_INTERVAL, 1, 6, 1);
	obs_property_set_long_description(interval, T_PROCESSING_INTERVAL_HINT);

	obs_property_set_visible(warning, runtime_warning_visible((struct segmentation_filter *)data));
	if (data) {
		struct segmentation_filter *filter = (struct segmentation_filter *)data;
		obs_data_t *settings = obs_source_get_settings(filter->context);
		output_mode_modified(props, nullptr, settings);
		obs_data_release(settings);
	} else {
		obs_property_set_visible(obs_properties_get(props, S_BACKGROUND_COLOR), false);
		obs_property_set_visible(obs_properties_get(props, S_BLUR_RADIUS), false);
	}

	dstr_free(&libraryFilter);
	dstr_free(&modelFilter);
	return props;
}

static void *filter_create(obs_data_t *settings, obs_source_t *context)
{
	struct segmentation_filter *filter = new segmentation_filter();
	filter->context = context;

	char *effectPath = obs_module_file("mediapipe_segmentation.effect");

	obs_enter_graphics();
	filter->effect = gs_effect_create_from_file(effectPath, nullptr);
	if (filter->effect) {
		filter->imageParam = gs_effect_get_param_by_name(filter->effect, "image");
		filter->maskParam = gs_effect_get_param_by_name(filter->effect, "mask");
		filter->backgroundColorParam = gs_effect_get_param_by_name(filter->effect, "background_color");
		filter->pixelSizeParam = gs_effect_get_param_by_name(filter->effect, "pixel_size");
		filter->thresholdParam = gs_effect_get_param_by_name(filter->effect, "threshold");
		filter->softnessParam = gs_effect_get_param_by_name(filter->effect, "softness");
		filter->blurRadiusParam = gs_effect_get_param_by_name(filter->effect, "blur_radius");
	}
	obs_leave_graphics();

	bfree(effectPath);

	if (!filter->effect) {
		delete filter;
		return nullptr;
	}

	filter_update(filter, settings);
	return filter;
}

static void filter_destroy(void *data)
{
	struct segmentation_filter *filter = (struct segmentation_filter *)data;

	if (!filter)
		return;

	runtime_unload(filter->runtime);
	destroy_graphics_resources(filter);

	obs_enter_graphics();
	gs_effect_destroy(filter->effect);
	obs_leave_graphics();

	delete filter;
}

static bool render_source_to_texture(struct segmentation_filter *filter, obs_source_t *target, obs_source_t *parent,
				     uint32_t width, uint32_t height)
{
	gs_texrender_reset(filter->render);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	bool rendered = false;
	if (gs_texrender_begin_with_color_space(filter->render, width, height, GS_CS_SRGB)) {
		struct vec4 clearColor;
		vec4_zero(&clearColor);
		gs_clear(GS_CLEAR_COLOR, &clearColor, 0.0f, 0);
		gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);

		const uint32_t targetFlags = obs_source_get_output_flags(target);
		const bool customDraw = (targetFlags & OBS_SOURCE_CUSTOM_DRAW) != 0;
		const bool async = (targetFlags & OBS_SOURCE_ASYNC) != 0;

		if (target == parent && !customDraw && !async)
			obs_source_default_render(target);
		else
			obs_source_video_render(target);

		gs_texrender_end(filter->render);
		rendered = true;
	}

	gs_blend_state_pop();
	return rendered;
}

static bool process_current_frame(struct segmentation_filter *filter)
{
	if (!runtime_ensure_instance(filter, filter->width, filter->height))
		return false;

	gs_stage_texture(filter->stage, gs_texrender_get_texture(filter->render));

	uint8_t *frameData = nullptr;
	uint32_t frameLinesize = 0;
	if (!gs_stagesurface_map(filter->stage, &frameData, &frameLinesize))
		return false;

	const bool processed = filter->runtime.processRgba(filter->runtime.instance, frameData, filter->width,
							   filter->height, frameLinesize, filter->mask.data(),
							   filter->width);
	gs_stagesurface_unmap(filter->stage);

	if (!processed)
		return false;

	gs_texture_set_image(filter->maskTexture, filter->mask.data(), filter->width, false);
	filter->maskReady = true;
	return true;
}

static const char *technique_for_mode(output_mode mode)
{
	switch (mode) {
	case OUTPUT_GREEN_SCREEN:
		return "GreenScreen";
	case OUTPUT_BLUR:
		return "Blur";
	case OUTPUT_MASK_PREVIEW:
		return "MaskPreview";
	case OUTPUT_TRANSPARENT:
	default:
		return "Transparent";
	}
}

static void draw_filtered_output(struct segmentation_filter *filter)
{
	if (!obs_source_process_filter_begin_with_color_space(filter->context, GS_RGBA, GS_CS_SRGB,
							     OBS_ALLOW_DIRECT_RENDERING))
		return;

	struct vec2 pixelSize;
	vec2_set(&pixelSize, 1.0f / (float)filter->width, 1.0f / (float)filter->height);

	gs_effect_set_texture_srgb(filter->imageParam, gs_texrender_get_texture(filter->render));
	gs_effect_set_texture(filter->maskParam, filter->maskTexture);
	gs_effect_set_vec4(filter->backgroundColorParam, &filter->backgroundColor);
	gs_effect_set_vec2(filter->pixelSizeParam, &pixelSize);
	gs_effect_set_float(filter->thresholdParam, filter->threshold);
	gs_effect_set_float(filter->softnessParam, filter->softness);
	gs_effect_set_float(filter->blurRadiusParam, filter->blurRadius);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	obs_source_process_filter_tech_end(filter->context, filter->effect, 0, 0, technique_for_mode(filter->mode));
	gs_blend_state_pop();
}

static void filter_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);

	struct segmentation_filter *filter = (struct segmentation_filter *)data;
	obs_source_t *target = obs_filter_get_target(filter->context);
	obs_source_t *parent = obs_filter_get_parent(filter->context);

	if (!target || !filter->effect) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	const uint32_t width = obs_source_get_base_width(target);
	const uint32_t height = obs_source_get_base_height(target);
	if (!width || !height) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	if (!ensure_graphics_resources(filter, width, height)) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	if (!render_source_to_texture(filter, target, parent, width, height)) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	const bool shouldProcess = !filter->maskReady || filter->processingCounter <= 0;
	if (shouldProcess) {
		if (!process_current_frame(filter)) {
			obs_source_skip_video_filter(filter->context);
			return;
		}
		filter->processingCounter = filter->processingInterval - 1;
	} else {
		filter->processingCounter--;
	}

	draw_filtered_output(filter);
}

static enum gs_color_space filter_get_color_space(void *data, size_t count,
						  const enum gs_color_space *preferred_spaces)
{
	UNUSED_PARAMETER(data);

	for (size_t i = 0; i < count; ++i) {
		if (preferred_spaces[i] == GS_CS_SRGB)
			return GS_CS_SRGB;
	}

	return GS_CS_SRGB;
}

bool obs_module_load(void)
{
	obs_source_info mediapipe_segmentation_filter = {};
	mediapipe_segmentation_filter.id = "mediapipe_segmentation_filter";
	mediapipe_segmentation_filter.type = OBS_SOURCE_TYPE_FILTER;
	mediapipe_segmentation_filter.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	mediapipe_segmentation_filter.get_name = filter_name;
	mediapipe_segmentation_filter.create = filter_create;
	mediapipe_segmentation_filter.destroy = filter_destroy;
	mediapipe_segmentation_filter.update = filter_update;
	mediapipe_segmentation_filter.get_defaults = filter_defaults;
	mediapipe_segmentation_filter.get_properties = filter_properties;
	mediapipe_segmentation_filter.video_render = filter_render;
	mediapipe_segmentation_filter.video_get_color_space = filter_get_color_space;

	obs_register_source(&mediapipe_segmentation_filter);
	return true;
}
