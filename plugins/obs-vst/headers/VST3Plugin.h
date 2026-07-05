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

#ifndef OBS_STUDIO_VST3PLUGIN_H
#define OBS_STUDIO_VST3PLUGIN_H

#ifndef BLOCK_SIZE
#define BLOCK_SIZE 512
#endif

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include <obs-module.h>

#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

class VST3Plugin {
	std::recursive_mutex lockEffect;
	std::atomic_bool effectReady = false;

	VST3::Hosting::Module::Ptr module;
	Steinberg::IPtr<Steinberg::Vst::PlugProvider> plugProvider;
	Steinberg::IPtr<Steinberg::Vst::IComponent> component;
	Steinberg::IPtr<Steinberg::Vst::IEditController> controller;
	Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> processor;

	Steinberg::Vst::HostProcessData processData;
	Steinberg::Vst::ProcessContext processContext = {};
	Steinberg::Vst::EventList eventList;
	Steinberg::Vst::ParameterChanges inputParameterChanges;
	Steinberg::Vst::TSamples processedSamples = 0;

	std::string pluginPath;
	std::string effectName;
	std::string vendorString;

	float **inputs = nullptr;
	float **outputs = nullptr;
	size_t numChannels = 0;
	int inputChannelCount = 0;
	int outputChannelCount = 0;

	void createChannelBuffers(size_t count);
	void cleanupChannelBuffers();
	bool setupBusArrangements();
	bool setupProcessing();
	void resetProcessContext(double sampleRate);

public:
	VST3Plugin() = default;
	~VST3Plugin();

	bool load(const std::string &path);
	void unload();
	bool loaded() const;

	obs_audio_data *process(struct obs_audio_data *audio);
	std::string getState();
	void setState(const std::string &data);

	std::string getEffectPath() const;
	std::string getEffectName() const;
};

#endif // OBS_STUDIO_VST3PLUGIN_H
