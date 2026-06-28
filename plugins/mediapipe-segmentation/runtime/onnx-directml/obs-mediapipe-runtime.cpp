#include "../mediapipe-runtime.h"

#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#define EXPORT_API extern "C" __declspec(dllexport)
#else
#define EXPORT_API extern "C"
#endif

namespace {

constexpr uint32_t kRuntimeVersion = 1;
constexpr size_t kPersonClassIndex = 1;

struct ModelLayout {
	enum class Layout {
		Unknown,
		Nchw,
		Nhwc,
	} layout = Layout::Unknown;

	int64_t width = 0;
	int64_t height = 0;
};

struct OutputLayout {
	enum class Layout {
		SingleHw,
		SingleNhw,
		SingleNchw,
		SingleNhwc,
		ClassesChw,
		ClassesHwc,
	} layout = Layout::SingleHw;

	int64_t width = 0;
	int64_t height = 0;
	int64_t classes = 1;
};

struct Runtime {
	Ort::Env env;
	Ort::SessionOptions sessionOptions;
	std::unique_ptr<Ort::Session> session;
	Ort::AllocatorWithDefaultOptions allocator;
	Ort::MemoryInfo memoryInfo;
	std::string inputName;
	std::string outputName;
	std::vector<const char *> inputNames;
	std::vector<const char *> outputNames;
	std::vector<int64_t> inputShape;
	ModelLayout inputLayout;
	std::vector<float> inputTensor;

	Runtime()
		: env(ORT_LOGGING_LEVEL_WARNING, "obs-mediapipe-runtime"),
		  memoryInfo(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault))
	{
	}
};

static void set_error(char *error, size_t errorSize, const char *message)
{
	if (!error || errorSize == 0)
		return;

	strncpy_s(error, errorSize, message ? message : "Unknown error", _TRUNCATE);
}

static std::wstring utf8_to_wide(const char *text)
{
	if (!text || !*text)
		return {};

	const int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
	if (count <= 0)
		throw std::runtime_error("Failed to convert model path to UTF-16");

	std::wstring wide((size_t)count - 1, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), count);
	return wide;
}

static int64_t require_static_dim(int64_t value, const char *name)
{
	if (value <= 0) {
		std::string message = "Model uses a dynamic or invalid ";
		message += name;
		message += " dimension; use a fixed-shape segmentation ONNX model";
		throw std::runtime_error(message);
	}

	return value;
}

static ModelLayout parse_input_layout(const std::vector<int64_t> &shape)
{
	if (shape.size() != 4)
		throw std::runtime_error("Expected a 4D float input tensor");

	ModelLayout layout;

	if (shape[1] == 3) {
		layout.layout = ModelLayout::Layout::Nchw;
		layout.height = require_static_dim(shape[2], "input height");
		layout.width = require_static_dim(shape[3], "input width");
	} else if (shape[3] == 3) {
		layout.layout = ModelLayout::Layout::Nhwc;
		layout.height = require_static_dim(shape[1], "input height");
		layout.width = require_static_dim(shape[2], "input width");
	} else {
		throw std::runtime_error("Expected RGB input layout NCHW or NHWC");
	}

	return layout;
}

static float clamp01(float value)
{
	return std::max(0.0f, std::min(1.0f, value));
}

static float normalize_score(float value)
{
	if (value >= 0.0f && value <= 1.0f)
		return value;

	return 1.0f / (1.0f + std::exp(-value));
}

static float sample_channel(const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t linesize, float x, float y,
			    uint32_t channel)
{
	x = std::max(0.0f, std::min((float)(width - 1), x));
	y = std::max(0.0f, std::min((float)(height - 1), y));

	const uint32_t x0 = (uint32_t)x;
	const uint32_t y0 = (uint32_t)y;
	const uint32_t x1 = std::min(x0 + 1, width - 1);
	const uint32_t y1 = std::min(y0 + 1, height - 1);
	const float tx = x - (float)x0;
	const float ty = y - (float)y0;

	const auto pixel = [&](uint32_t px, uint32_t py) -> float {
		return (float)rgba[(size_t)py * linesize + (size_t)px * 4 + channel] / 255.0f;
	};

	const float a = pixel(x0, y0) * (1.0f - tx) + pixel(x1, y0) * tx;
	const float b = pixel(x0, y1) * (1.0f - tx) + pixel(x1, y1) * tx;
	return a * (1.0f - ty) + b * ty;
}

static void fill_input_tensor(Runtime *runtime, const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t linesize)
{
	const int64_t inputWidth = runtime->inputLayout.width;
	const int64_t inputHeight = runtime->inputLayout.height;
	const size_t pixelCount = (size_t)inputWidth * (size_t)inputHeight;
	runtime->inputTensor.assign(pixelCount * 3, 0.0f);

	for (int64_t y = 0; y < inputHeight; ++y) {
		const float srcY = ((float)y + 0.5f) * (float)height / (float)inputHeight - 0.5f;

		for (int64_t x = 0; x < inputWidth; ++x) {
			const float srcX = ((float)x + 0.5f) * (float)width / (float)inputWidth - 0.5f;
			const size_t pixelIndex = (size_t)y * (size_t)inputWidth + (size_t)x;

			if (runtime->inputLayout.layout == ModelLayout::Layout::Nchw) {
				runtime->inputTensor[pixelIndex] = sample_channel(rgba, width, height, linesize, srcX, srcY, 0);
				runtime->inputTensor[pixelCount + pixelIndex] =
					sample_channel(rgba, width, height, linesize, srcX, srcY, 1);
				runtime->inputTensor[pixelCount * 2 + pixelIndex] =
					sample_channel(rgba, width, height, linesize, srcX, srcY, 2);
			} else {
				const size_t base = pixelIndex * 3;
				runtime->inputTensor[base] = sample_channel(rgba, width, height, linesize, srcX, srcY, 0);
				runtime->inputTensor[base + 1] =
					sample_channel(rgba, width, height, linesize, srcX, srcY, 1);
				runtime->inputTensor[base + 2] =
					sample_channel(rgba, width, height, linesize, srcX, srcY, 2);
			}
		}
	}
}

static OutputLayout parse_output_layout(const std::vector<int64_t> &shape)
{
	OutputLayout output;

	if (shape.size() == 2) {
		output.layout = OutputLayout::Layout::SingleHw;
		output.height = require_static_dim(shape[0], "output height");
		output.width = require_static_dim(shape[1], "output width");
	} else if (shape.size() == 3) {
		if (shape[0] == 1) {
			output.layout = OutputLayout::Layout::SingleNhw;
			output.height = require_static_dim(shape[1], "output height");
			output.width = require_static_dim(shape[2], "output width");
		} else {
			output.layout = OutputLayout::Layout::ClassesChw;
			output.classes = require_static_dim(shape[0], "output classes");
			output.height = require_static_dim(shape[1], "output height");
			output.width = require_static_dim(shape[2], "output width");
		}
	} else if (shape.size() == 4) {
		if (shape[1] > 1) {
			output.layout = OutputLayout::Layout::ClassesChw;
			output.classes = require_static_dim(shape[1], "output classes");
			output.height = require_static_dim(shape[2], "output height");
			output.width = require_static_dim(shape[3], "output width");
		} else if (shape[3] > 1) {
			output.layout = OutputLayout::Layout::ClassesHwc;
			output.height = require_static_dim(shape[1], "output height");
			output.width = require_static_dim(shape[2], "output width");
			output.classes = require_static_dim(shape[3], "output classes");
		} else if (shape[1] == 1) {
			output.layout = OutputLayout::Layout::SingleNchw;
			output.height = require_static_dim(shape[2], "output height");
			output.width = require_static_dim(shape[3], "output width");
		} else {
			output.layout = OutputLayout::Layout::SingleNhwc;
			output.height = require_static_dim(shape[1], "output height");
			output.width = require_static_dim(shape[2], "output width");
		}
	} else {
		throw std::runtime_error("Unsupported output tensor rank");
	}

	if (output.classes < 1)
		throw std::runtime_error("Invalid output class count");

	return output;
}

static float output_value(const float *data, const OutputLayout &layout, int64_t x, int64_t y)
{
	switch (layout.layout) {
	case OutputLayout::Layout::SingleHw:
	case OutputLayout::Layout::SingleNhw:
	case OutputLayout::Layout::SingleNchw:
	case OutputLayout::Layout::SingleNhwc:
		return normalize_score(data[(size_t)y * (size_t)layout.width + (size_t)x]);
	case OutputLayout::Layout::ClassesChw: {
		const size_t planeSize = (size_t)layout.width * (size_t)layout.height;
		const size_t classIndex = std::min((size_t)layout.classes - 1, kPersonClassIndex);
		float maxValue = data[(size_t)y * (size_t)layout.width + (size_t)x];
		for (int64_t c = 1; c < layout.classes; ++c)
			maxValue = std::max(maxValue, data[(size_t)c * planeSize + (size_t)y * (size_t)layout.width +
							      (size_t)x]);
		float denom = 0.0f;
		for (int64_t c = 0; c < layout.classes; ++c)
			denom += std::exp(data[(size_t)c * planeSize + (size_t)y * (size_t)layout.width + (size_t)x] -
					  maxValue);
		const float selected =
			std::exp(data[classIndex * planeSize + (size_t)y * (size_t)layout.width + (size_t)x] - maxValue);
		return denom > 0.0f ? selected / denom : 0.0f;
	}
	case OutputLayout::Layout::ClassesHwc: {
		const size_t classIndex = std::min((size_t)layout.classes - 1, kPersonClassIndex);
		const size_t base = ((size_t)y * (size_t)layout.width + (size_t)x) * (size_t)layout.classes;
		float maxValue = data[base];
		for (int64_t c = 1; c < layout.classes; ++c)
			maxValue = std::max(maxValue, data[base + (size_t)c]);
		float denom = 0.0f;
		for (int64_t c = 0; c < layout.classes; ++c)
			denom += std::exp(data[base + (size_t)c] - maxValue);
		const float selected = std::exp(data[base + classIndex] - maxValue);
		return denom > 0.0f ? selected / denom : 0.0f;
	}
	}

	return 0.0f;
}

static float sample_mask_bilinear(const float *data, const OutputLayout &layout, float x, float y)
{
	x = std::max(0.0f, std::min((float)(layout.width - 1), x));
	y = std::max(0.0f, std::min((float)(layout.height - 1), y));

	const int64_t x0 = (int64_t)x;
	const int64_t y0 = (int64_t)y;
	const int64_t x1 = std::min(x0 + 1, layout.width - 1);
	const int64_t y1 = std::min(y0 + 1, layout.height - 1);
	const float tx = x - (float)x0;
	const float ty = y - (float)y0;

	const float a = output_value(data, layout, x0, y0) * (1.0f - tx) + output_value(data, layout, x1, y0) * tx;
	const float b = output_value(data, layout, x0, y1) * (1.0f - tx) + output_value(data, layout, x1, y1) * tx;
	return clamp01(a * (1.0f - ty) + b * ty);
}

static void write_mask(const float *data, const OutputLayout &layout, uint8_t *mask, uint32_t width, uint32_t height,
		       uint32_t linesize)
{
	for (uint32_t y = 0; y < height; ++y) {
		const float srcY = ((float)y + 0.5f) * (float)layout.height / (float)height - 0.5f;

		for (uint32_t x = 0; x < width; ++x) {
			const float srcX = ((float)x + 0.5f) * (float)layout.width / (float)width - 0.5f;
			const float value = sample_mask_bilinear(data, layout, srcX, srcY);
			mask[(size_t)y * linesize + x] = (uint8_t)std::lround(value * 255.0f);
		}
	}
}

static void enable_directml_or_cpu(Ort::SessionOptions &options)
{
	options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
	options.DisableMemPattern();
	options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);

	const OrtApi &api = Ort::GetApi();
	OrtStatus *status = OrtSessionOptionsAppendExecutionProvider_DML(options, 0);
	if (status) {
		api.ReleaseStatus(status);
	}
}

} // namespace

EXPORT_API uint32_t obs_mediapipe_runtime_version(void)
{
	return kRuntimeVersion;
}

EXPORT_API obs_mediapipe_runtime_t obs_mediapipe_runtime_create(const char *modelPath, uint32_t width, uint32_t height,
								char *error, size_t errorSize)
{
	try {
		if (!modelPath || !*modelPath)
			throw std::runtime_error("Model path is empty");
		if (!width || !height)
			throw std::runtime_error("Invalid source dimensions");

		std::unique_ptr<Runtime> runtime = std::make_unique<Runtime>();
		enable_directml_or_cpu(runtime->sessionOptions);

#ifdef _WIN32
		const std::wstring wideModelPath = utf8_to_wide(modelPath);
		runtime->session = std::make_unique<Ort::Session>(runtime->env, wideModelPath.c_str(),
								  runtime->sessionOptions);
#else
		runtime->session = std::make_unique<Ort::Session>(runtime->env, modelPath, runtime->sessionOptions);
#endif

		Ort::AllocatedStringPtr inputName = runtime->session->GetInputNameAllocated(0, runtime->allocator);
		Ort::AllocatedStringPtr outputName = runtime->session->GetOutputNameAllocated(0, runtime->allocator);
		runtime->inputName = inputName.get();
		runtime->outputName = outputName.get();
		runtime->inputNames = {runtime->inputName.c_str()};
		runtime->outputNames = {runtime->outputName.c_str()};

		Ort::TypeInfo inputType = runtime->session->GetInputTypeInfo(0);
		auto inputTensorInfo = inputType.GetTensorTypeAndShapeInfo();
		if (inputTensorInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
			throw std::runtime_error("Only float32 ONNX inputs are supported");

		runtime->inputShape = inputTensorInfo.GetShape();
		runtime->inputLayout = parse_input_layout(runtime->inputShape);

		return runtime.release();
	} catch (const std::exception &ex) {
		set_error(error, errorSize, ex.what());
		return nullptr;
	}
}

EXPORT_API void obs_mediapipe_runtime_destroy(obs_mediapipe_runtime_t runtime)
{
	delete static_cast<Runtime *>(runtime);
}

EXPORT_API bool obs_mediapipe_runtime_process_rgba(obs_mediapipe_runtime_t handle, const uint8_t *rgba, uint32_t width,
						   uint32_t height, uint32_t rgbaLinesize, uint8_t *mask,
						   uint32_t maskLinesize)
{
	try {
		Runtime *runtime = static_cast<Runtime *>(handle);
		if (!runtime || !rgba || !mask)
			return false;

		fill_input_tensor(runtime, rgba, width, height, rgbaLinesize);

		Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
			runtime->memoryInfo, runtime->inputTensor.data(), runtime->inputTensor.size(),
			runtime->inputShape.data(), runtime->inputShape.size());

		std::vector<Ort::Value> outputs =
			runtime->session->Run(Ort::RunOptions{nullptr}, runtime->inputNames.data(), &inputTensor, 1,
					      runtime->outputNames.data(), 1);

		if (outputs.empty() || !outputs[0].IsTensor())
			return false;

		auto outputInfo = outputs[0].GetTensorTypeAndShapeInfo();
		if (outputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
			return false;

		const OutputLayout outputLayout = parse_output_layout(outputInfo.GetShape());
		const float *outputData = outputs[0].GetTensorData<float>();
		write_mask(outputData, outputLayout, mask, width, height, maskLinesize);
		return true;
	} catch (...) {
		return false;
	}
}
