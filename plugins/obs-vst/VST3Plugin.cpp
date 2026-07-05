/*****************************************************************************
Copyright (C) 2026 by OBS Studio contributors

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
*****************************************************************************/

#include "headers/VST3Plugin.h"

#include <QByteArray>
#include <QDataStream>
#include <QIODevice>
#include <QString>

#include <algorithm>
#include <cstring>
#include <mutex>

#include <util/platform.h>

#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/utility/memoryibstream.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"

#define VST3_STATE_MAGIC "OBSVST31"
#define VST3_MAX_CHANNELS 256

namespace {

class OBSVST3HostApplication final : public Steinberg::Vst::HostApplication {
public:
	Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override
	{
		return Steinberg::Vst::StringConvert::convert("OBS Studio", name) ? Steinberg::kResultTrue
										       : Steinberg::kInternalError;
	}
};

Steinberg::FUnknown *getHostContext()
{
	static OBSVST3HostApplication hostApplication;
	static std::once_flag initFlag;

	std::call_once(initFlag, [] {
		Steinberg::Vst::PluginContextFactory::instance().setPluginContext(&hostApplication);
		Steinberg::Vst::PlugProvider::setErrorStream(nullptr);
	});

	return &hostApplication;
}

static void silenceChannel(float **channelData, size_t numChannels, long numFrames)
{
	for (size_t channel = 0; channel < numChannels; ++channel) {
		for (long frame = 0; frame < numFrames; ++frame) {
			channelData[channel][frame] = 0.0f;
		}
	}
}

static QByteArray takeStreamBytes(Steinberg::ResizableMemoryIBStream &stream)
{
	std::vector<Steinberg::uint8> data = stream.take();
	if (data.empty())
		return QByteArray();

	return QByteArray(reinterpret_cast<const char *>(data.data()), static_cast<int>(data.size()));
}

static void fillStream(Steinberg::ResizableMemoryIBStream &stream, const QByteArray &data)
{
	if (data.isEmpty())
		return;

	stream.write(const_cast<char *>(data.constData()), data.size(), nullptr);
	stream.rewind();
}

} // namespace

VST3Plugin::~VST3Plugin()
{
	unload();
	cleanupChannelBuffers();
}

void VST3Plugin::createChannelBuffers(size_t count)
{
	cleanupChannelBuffers();

	numChannels = count;
	if (numChannels == 0)
		return;

	inputs = static_cast<float **>(bmalloc(sizeof(float *) * numChannels));
	outputs = static_cast<float **>(bmalloc(sizeof(float *) * numChannels));

	for (size_t channel = 0; channel < numChannels; channel++) {
		inputs[channel] = static_cast<float *>(bmalloc(sizeof(float) * BLOCK_SIZE));
		outputs[channel] = static_cast<float *>(bmalloc(sizeof(float) * BLOCK_SIZE));
	}
}

void VST3Plugin::cleanupChannelBuffers()
{
	for (size_t channel = 0; channel < numChannels; channel++) {
		if (inputs && inputs[channel]) {
			bfree(inputs[channel]);
			inputs[channel] = nullptr;
		}
		if (outputs && outputs[channel]) {
			bfree(outputs[channel]);
			outputs[channel] = nullptr;
		}
	}

	if (inputs) {
		bfree(inputs);
		inputs = nullptr;
	}
	if (outputs) {
		bfree(outputs);
		outputs = nullptr;
	}

	numChannels = 0;
}

void VST3Plugin::resetProcessContext(double sampleRate)
{
	processContext = {};
	processContext.sampleRate = sampleRate;
	processContext.tempo = 120.0;
	processContext.timeSigNumerator = 4;
	processContext.timeSigDenominator = 4;
	processContext.state = Steinberg::Vst::ProcessContext::kPlaying |
			       Steinberg::Vst::ProcessContext::kSystemTimeValid |
			       Steinberg::Vst::ProcessContext::kContTimeValid |
			       Steinberg::Vst::ProcessContext::kTempoValid |
			       Steinberg::Vst::ProcessContext::kTimeSigValid;
}

bool VST3Plugin::setupBusArrangements()
{
	if (!component || !processor)
		return false;

	const Steinberg::int32 inputBusCount = component->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kInput);
	const Steinberg::int32 outputBusCount = component->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput);
	if (inputBusCount <= 0 || outputBusCount <= 0) {
		blog(LOG_WARNING, "VST3 Plug-in has no main audio input/output bus: '%s'", pluginPath.c_str());
		return false;
	}

	for (Steinberg::int32 bus = 0; bus < inputBusCount; ++bus)
		component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, bus, bus == 0);
	for (Steinberg::int32 bus = 0; bus < outputBusCount; ++bus)
		component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, bus, bus == 0);

	Steinberg::Vst::SpeakerArrangement inputArrangement = Steinberg::Vst::SpeakerArr::kStereo;
	Steinberg::Vst::SpeakerArrangement outputArrangement = Steinberg::Vst::SpeakerArr::kStereo;

	if (processor->setBusArrangements(&inputArrangement, 1, &outputArrangement, 1) != Steinberg::kResultOk) {
		inputArrangement = Steinberg::Vst::SpeakerArr::kMono;
		outputArrangement = Steinberg::Vst::SpeakerArr::kMono;
		if (processor->setBusArrangements(&inputArrangement, 1, &outputArrangement, 1) != Steinberg::kResultOk) {
			processor->getBusArrangement(Steinberg::Vst::kInput, 0, inputArrangement);
			processor->getBusArrangement(Steinberg::Vst::kOutput, 0, outputArrangement);
		}
	}

	Steinberg::Vst::BusInfo inputInfo = {};
	Steinberg::Vst::BusInfo outputInfo = {};
	if (component->getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, 0, inputInfo) != Steinberg::kResultOk ||
	    component->getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, 0, outputInfo) != Steinberg::kResultOk) {
		blog(LOG_WARNING, "VST3 Plug-in has invalid bus information: '%s'", pluginPath.c_str());
		return false;
	}

	inputChannelCount = inputInfo.channelCount;
	outputChannelCount = outputInfo.channelCount;
	const int maxChannels = std::max(inputChannelCount, outputChannelCount);
	if (maxChannels <= 0 || maxChannels > VST3_MAX_CHANNELS) {
		blog(LOG_WARNING, "VST3 Plug-in has invalid number of channels: '%s'", pluginPath.c_str());
		return false;
	}

	createChannelBuffers(static_cast<size_t>(maxChannels));
	return true;
}

bool VST3Plugin::setupProcessing()
{
	if (!component || !processor)
		return false;

	const double sampleRate = static_cast<double>(audio_output_get_sample_rate(obs_get_audio()));
	resetProcessContext(sampleRate);
	processedSamples = 0;

	processData.inputEvents = &eventList;
	processData.inputParameterChanges = &inputParameterChanges;
	processData.processContext = &processContext;
	processData.prepare(*component, 0, Steinberg::Vst::kSample32);

	Steinberg::Vst::ProcessSetup setup = {};
	setup.processMode = Steinberg::Vst::kRealtime;
	setup.symbolicSampleSize = Steinberg::Vst::kSample32;
	setup.maxSamplesPerBlock = BLOCK_SIZE;
	setup.sampleRate = sampleRate;

	if (processor->setupProcessing(setup) != Steinberg::kResultOk) {
		blog(LOG_WARNING, "VST3 Plug-in failed setupProcessing: '%s'", pluginPath.c_str());
		return false;
	}
	if (component->setActive(true) != Steinberg::kResultOk) {
		blog(LOG_WARNING, "VST3 Plug-in failed setActive(true): '%s'", pluginPath.c_str());
		return false;
	}
	if (processor->setProcessing(true) != Steinberg::kResultOk) {
		blog(LOG_WARNING, "VST3 Plug-in failed setProcessing(true): '%s'", pluginPath.c_str());
		return false;
	}

	return true;
}

bool VST3Plugin::load(const std::string &path)
{
	unload();

	std::lock_guard<std::recursive_mutex> lock(lockEffect);

	pluginPath = path;
	std::string error;
	module = VST3::Hosting::Module::create(path, error);
	if (!module) {
		blog(LOG_WARNING, "VST3 Plug-in: Can't load module '%s': %s", path.c_str(), error.c_str());
		pluginPath.clear();
		return false;
	}

	auto factory = module->getFactory();
	factory.setHostContext(getHostContext());

	VST3::Hosting::ClassInfo selectedClass;
	bool foundAudioEffect = false;
	for (const auto &classInfo : factory.classInfos()) {
		if (classInfo.category() == kVstAudioEffectClass) {
			selectedClass = classInfo;
			foundAudioEffect = true;
			break;
		}
	}

	if (!foundAudioEffect) {
		blog(LOG_WARNING, "VST3 Plug-in: No audio effect class found in '%s'", path.c_str());
		unload();
		return false;
	}

	plugProvider = Steinberg::owned(new Steinberg::Vst::PlugProvider(factory, selectedClass, true));
	if (!plugProvider || !plugProvider->initialize()) {
		blog(LOG_WARNING, "VST3 Plug-in: Failed to initialize '%s'", path.c_str());
		unload();
		return false;
	}

	component = plugProvider->getComponentPtr();
	controller = plugProvider->getControllerPtr();
	processor = Steinberg::U::cast<Steinberg::Vst::IAudioProcessor>(component);
	if (!component || !processor) {
		blog(LOG_WARNING, "VST3 Plug-in: Selected class is not an audio processor '%s'", path.c_str());
		unload();
		return false;
	}

	effectName = selectedClass.name();
	vendorString = selectedClass.vendor();

	if (!setupBusArrangements() || !setupProcessing()) {
		unload();
		return false;
	}

	effectReady = true;
	blog(LOG_INFO, "VST3 Plug-in loaded: '%s' by '%s'", effectName.c_str(), vendorString.c_str());
	return true;
}

void VST3Plugin::unload()
{
	std::lock_guard<std::recursive_mutex> lock(lockEffect);

	effectReady = false;

	if (processor)
		processor->setProcessing(false);
	if (component)
		component->setActive(false);

	processData.unprepare();
	eventList.clear();
	inputParameterChanges.clearQueue();

	processor = nullptr;
	controller = nullptr;
	component = nullptr;
	plugProvider = nullptr;
	module = nullptr;

	pluginPath.clear();
	effectName.clear();
	vendorString.clear();
	inputChannelCount = 0;
	outputChannelCount = 0;
	processedSamples = 0;

	cleanupChannelBuffers();
}

bool VST3Plugin::loaded() const
{
	return effectReady && processor;
}

obs_audio_data *VST3Plugin::process(struct obs_audio_data *audio)
{
	if (!audio || !effectReady || !processor || numChannels == 0)
		return audio;

	std::lock_guard<std::recursive_mutex> lock(lockEffect);
	if (!effectReady || !processor || numChannels == 0)
		return audio;

	uint passes = (audio->frames + BLOCK_SIZE - 1) / BLOCK_SIZE;
	uint extra = audio->frames % BLOCK_SIZE;

	for (uint pass = 0; pass < passes; pass++) {
		uint frames = pass == passes - 1 && extra ? extra : BLOCK_SIZE;
		silenceChannel(inputs, numChannels, BLOCK_SIZE);
		silenceChannel(outputs, numChannels, BLOCK_SIZE);

		for (int c = 0; c < inputChannelCount; c++) {
			float *inputBuffer = inputs[c];
			if (c < MAX_AV_PLANES && audio->data[c] != nullptr)
				inputBuffer = reinterpret_cast<float *>(audio->data[c]) + (pass * BLOCK_SIZE);
			processData.setChannelBuffer(Steinberg::Vst::kInput, 0, c, inputBuffer);
		}

		for (int c = 0; c < outputChannelCount; c++)
			processData.setChannelBuffer(Steinberg::Vst::kOutput, 0, c, outputs[c]);

		processContext.projectTimeSamples = processedSamples;
		processContext.continousTimeSamples = processedSamples;
		processContext.systemTime = static_cast<Steinberg::int64>(os_gettime_ns());
		processData.numSamples = static_cast<Steinberg::int32>(frames);
		processedSamples += frames;

		processData.inputs[0].silenceFlags = 0;
		processData.outputs[0].silenceFlags = 0;

		if (processor->process(processData) != Steinberg::kResultOk)
			continue;

		for (int c = 0; c < outputChannelCount && c < MAX_AV_PLANES; c++) {
			if (!audio->data[c])
				continue;

			float *channel = reinterpret_cast<float *>(audio->data[c]) + (pass * BLOCK_SIZE);
			for (uint i = 0; i < frames; i++)
				channel[i] = outputs[c][i];
		}
	}

	eventList.clear();
	inputParameterChanges.clearQueue();
	return audio;
}

std::string VST3Plugin::getState()
{
	std::lock_guard<std::recursive_mutex> lock(lockEffect);
	if (!effectReady || !component)
		return "";

	QByteArray componentData;
	QByteArray controllerData;

	Steinberg::ResizableMemoryIBStream componentStream;
	if (component->getState(&componentStream) == Steinberg::kResultTrue)
		componentData = takeStreamBytes(componentStream);

	if (controller) {
		Steinberg::ResizableMemoryIBStream controllerStream;
		if (controller->getState(&controllerStream) == Steinberg::kResultTrue)
			controllerData = takeStreamBytes(controllerStream);
	}

	if (componentData.isEmpty() && controllerData.isEmpty())
		return "";

	QByteArray payload;
	QDataStream stream(&payload, QIODevice::WriteOnly);
	stream.setByteOrder(QDataStream::LittleEndian);
	stream.writeRawData(VST3_STATE_MAGIC, 8);
	stream << static_cast<quint32>(componentData.size());
	stream << static_cast<quint32>(controllerData.size());
	if (!componentData.isEmpty())
		stream.writeRawData(componentData.constData(), componentData.size());
	if (!controllerData.isEmpty())
		stream.writeRawData(controllerData.constData(), controllerData.size());

	return QString(payload.toBase64()).toStdString();
}

void VST3Plugin::setState(const std::string &data)
{
	std::lock_guard<std::recursive_mutex> lock(lockEffect);
	if (!effectReady || !component || data.empty())
		return;

	QByteArray payload = QByteArray::fromBase64(QByteArray(data.c_str(), static_cast<int>(data.length())));
	if (payload.size() < 16)
		return;

	QDataStream stream(&payload, QIODevice::ReadOnly);
	stream.setByteOrder(QDataStream::LittleEndian);

	char magic[8] = {};
	if (stream.readRawData(magic, 8) != 8 || std::memcmp(magic, VST3_STATE_MAGIC, 8) != 0)
		return;

	quint32 componentSize = 0;
	quint32 controllerSize = 0;
	stream >> componentSize;
	stream >> controllerSize;
	if (stream.status() != QDataStream::Ok)
		return;
	if (payload.size() < 16 + static_cast<int>(componentSize) + static_cast<int>(controllerSize))
		return;

	QByteArray componentData(static_cast<int>(componentSize), Qt::Uninitialized);
	QByteArray controllerData(static_cast<int>(controllerSize), Qt::Uninitialized);
	if (componentSize && stream.readRawData(componentData.data(), componentData.size()) != componentData.size())
		return;
	if (controllerSize && stream.readRawData(controllerData.data(), controllerData.size()) != controllerData.size())
		return;

	if (!componentData.isEmpty()) {
		Steinberg::ResizableMemoryIBStream componentStream(componentData.size());
		fillStream(componentStream, componentData);
		component->setState(&componentStream);

		if (controller) {
			componentStream.rewind();
			controller->setComponentState(&componentStream);
		}
	}

	if (controller && !controllerData.isEmpty()) {
		Steinberg::ResizableMemoryIBStream controllerStream(controllerData.size());
		fillStream(controllerStream, controllerData);
		controller->setState(&controllerStream);
	}
}

std::string VST3Plugin::getEffectPath() const
{
	return pluginPath;
}

std::string VST3Plugin::getEffectName() const
{
	return effectName;
}
