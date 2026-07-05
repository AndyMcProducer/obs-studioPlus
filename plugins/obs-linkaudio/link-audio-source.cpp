#include <obs-module.h>
#include <util/platform.h>
#include <util/util_uint64.h>

#include "AudioRingBuffer.h"
#include "LinkAudioManager.h"

#include <ableton/LinkAudio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#define blog(level, msg, ...) blog(level, "obs-linkaudio: " msg, ##__VA_ARGS__)

namespace {

constexpr const char *kLocalPeerName = "local_peer_name";
constexpr const char *kSourcePeerName = "source_peer_name";
constexpr const char *kSourceChannelName = "source_channel_name";
constexpr const char *kChannelCount = "channel_count";
constexpr const char *kBufferSize = "buffer_size";
constexpr std::size_t kRingSize = 32768;
constexpr uint64_t kStartupTimeoutNs = 500000000ULL;
constexpr uint64_t kNsPerSec = 1000000000ULL;

static uint32_t obs_audio_sample_rate()
{
	obs_audio_info2 info = {};
	if (obs_get_audio_info2(&info) && info.samples_per_sec != 0)
		return info.samples_per_sec;

	return 48000;
}

static enum speaker_layout speaker_layout_for_channels(int channels)
{
	return channels == 1 ? SPEAKERS_MONO : SPEAKERS_STEREO;
}

static uint64_t audio_timestamp(uint32_t frames, uint32_t sampleRate)
{
	return os_gettime_ns() - util_mul_div64(frames, kNsPerSec, sampleRate);
}

class LinkAudioSource {
public:
	LinkAudioSource(obs_data_t *settings, obs_source_t *source) : source(source) { update(settings); }

	~LinkAudioSource() { close(); }

	void update(obs_data_t *settings)
	{
		std::lock_guard<std::mutex> lock(settingsMutex);

		localPeerName = obs_data_get_string(settings, kLocalPeerName);
		sourcePeerName = obs_data_get_string(settings, kSourcePeerName);
		sourceChannelName = obs_data_get_string(settings, kSourceChannelName);
		channelCount = (int)obs_data_get_int(settings, kChannelCount);
		bufferSize = (int)obs_data_get_int(settings, kBufferSize);

		if (localPeerName.empty())
			localPeerName = "OBS";
		if (sourceChannelName.empty())
			sourceChannelName = "Main";
		if (channelCount != 1 && channelCount != 2)
			channelCount = 2;
		if (bufferSize < 64)
			bufferSize = 64;
		if (bufferSize > 4096)
			bufferSize = 4096;

		if (manager)
			manager->setPeerName(localPeerName);

		stateDirty.store(true);
		workerCv.notify_all();
	}

	void activate()
	{
		if (active.exchange(true))
			return;

		start();
	}

	void deactivate()
	{
		active.store(false);
		stopReceiving();
	}

private:
	void start()
	{
		std::lock_guard<std::mutex> lock(lifetimeMutex);

		if (running)
			return;

		manager = LinkAudioManager::acquire();
		manager->setPeerName(currentLocalPeerName());
		manager->linkAudio().enable(true);
		manager->linkAudio().enableLinkAudio(true);

		ringL.reset(new AudioRingBuffer(kRingSize));
		ringR.reset(new AudioRingBuffer(kRingSize));

		workerStop.store(false);
		audioThreadStop.store(false);
		workerThread = std::thread([this] { workerThreadLoop(); });
		audioThread = std::thread([this] { audioThreadLoop(); });
		running = true;
	}

	void close()
	{
		active.store(false);

		{
			std::lock_guard<std::mutex> lock(lifetimeMutex);
			if (!running)
				return;
			workerStop.store(true);
			audioThreadStop.store(true);
			workerCv.notify_all();
		}

		if (workerThread.joinable())
			workerThread.join();
		if (audioThread.joinable())
			audioThread.join();

		std::lock_guard<std::mutex> lock(lifetimeMutex);
		unsubscribe();
		ringL.reset();
		ringR.reset();
		manager.reset();
		running = false;
	}

	void stopReceiving()
	{
		std::lock_guard<std::mutex> lock(lifetimeMutex);
		unsubscribe();
		if (ringL)
			ringL->reset();
		if (ringR)
			ringR->reset();
		firstTimestamp = 0;
	}

	std::string currentLocalPeerName()
	{
		std::lock_guard<std::mutex> lock(settingsMutex);
		return localPeerName;
	}

	void workerThreadLoop()
	{
		using namespace std::chrono_literals;

		while (!workerStop.load()) {
			applyState();
			stateDirty.store(false);

			std::unique_lock<std::mutex> lock(workerCvMutex);
			workerCv.wait_for(lock, 200ms, [this] { return workerStop.load() || stateDirty.load(); });
		}
	}

	void applyState()
	{
		if (!manager || !active.load())
			return;

		std::string wantedPeer;
		std::string wantedChannel;
		std::string wantedLocalPeer;
		{
			std::lock_guard<std::mutex> lock(settingsMutex);
			wantedPeer = sourcePeerName;
			wantedChannel = sourceChannelName;
			wantedLocalPeer = localPeerName;
		}

		if (wantedChannel.empty()) {
			unsubscribe();
			return;
		}

		manager->setPeerName(wantedLocalPeer);

		if (wantedPeer != subscribedPeer || wantedChannel != subscribedChannel) {
			unsubscribe();
			subscribedPeer = wantedPeer;
			subscribedChannel = wantedChannel;
		}

		if (!sourceSubscription)
			trySubscribe(wantedPeer, wantedChannel);

		peerCount.store((int)manager->numPeers());
	}

	void trySubscribe(const std::string &wantedPeer, const std::string &wantedChannel)
	{
		auto channels = manager->channels();
		std::optional<ableton::ChannelId> match;
		std::string matchedPeer;

		for (const auto &channel : channels) {
			if (channel.name != wantedChannel)
				continue;
			if (!wantedPeer.empty() && channel.peerName != wantedPeer)
				continue;

			match = channel.id;
			matchedPeer = channel.peerName;
			break;
		}

		if (!match)
			return;

		if (ringL)
			ringL->reset();
		if (ringR)
			ringR->reset();
		framesReceived.store(0);
		framesDropped.store(0);
		firstTimestamp = 0;

		auto *left = ringL.get();
		auto *right = ringR.get();
		auto *sampleRate = &streamSampleRate;
		auto *streamChannels = &streamChannelCount;
		auto *received = &framesReceived;
		auto *dropped = &framesDropped;

		sourceSubscription.reset(new ableton::LinkAudioSource(
			manager->linkAudio(), *match,
			[left, right, sampleRate, streamChannels, received, dropped]
			(ableton::LinkAudioSource::BufferHandle buffer) {
				pushBuffer(buffer, *left, *right, *sampleRate, *streamChannels, *received, *dropped);
			}));

		blog(LOG_INFO, "Subscribed to Link Audio channel '%s' from peer '%s'", wantedChannel.c_str(),
		     matchedPeer.c_str());
	}

	static void pushBuffer(ableton::LinkAudioSource::BufferHandle &buffer, AudioRingBuffer &left,
			       AudioRingBuffer &right, std::atomic<uint32_t> &sampleRate,
			       std::atomic<uint32_t> &streamChannels, std::atomic<uint64_t> &received,
			       std::atomic<uint64_t> &dropped)
	{
		const auto &info = buffer.info;
		if (info.numFrames == 0 || info.numChannels == 0 || buffer.samples == nullptr)
			return;

		sampleRate.store(info.sampleRate);
		streamChannels.store((uint32_t)info.numChannels);

		constexpr std::size_t batch = 512;
		float scratchL[batch];
		float scratchR[batch];
		const bool stereo = info.numChannels >= 2;

		std::size_t framesLeft = info.numFrames;
		std::size_t offset = 0;

		while (framesLeft > 0) {
			const std::size_t count = std::min(framesLeft, batch);

			for (std::size_t i = 0; i < count; ++i) {
				const std::size_t base = (offset + i) * info.numChannels;
				scratchL[i] = (float)buffer.samples[base] / 32768.0f;
				scratchR[i] = stereo ? (float)buffer.samples[base + 1] / 32768.0f : scratchL[i];
			}

			const std::size_t written = left.write(scratchL, count);
			right.write(scratchR, count);
			if (written < count)
				dropped.fetch_add(count - written);

			offset += count;
			framesLeft -= count;
		}

		received.fetch_add(info.numFrames);
	}

	void unsubscribe()
	{
		if (sourceSubscription) {
			blog(LOG_INFO, "Unsubscribed from Link Audio channel '%s'", subscribedChannel.c_str());
			sourceSubscription.reset();
		}
	}

	void audioThreadLoop()
	{
		std::vector<float> left;
		std::vector<float> right;

		while (!audioThreadStop.load()) {
			int channels;
			int frames;
			{
				std::lock_guard<std::mutex> lock(settingsMutex);
				channels = channelCount;
				frames = bufferSize;
			}

			left.assign((std::size_t)frames, 0.0f);
			right.assign((std::size_t)frames, 0.0f);

			if (active.load() && sourceSubscription && ringL && ringR) {
				const std::size_t readL = ringL->read(left.data(), (std::size_t)frames);
				const std::size_t readR = ringR->read(right.data(), (std::size_t)frames);
				if (readL < (std::size_t)frames)
					std::fill(left.begin() + readL, left.end(), 0.0f);
				if (readR < (std::size_t)frames)
					std::fill(right.begin() + readR, right.end(), 0.0f);
			}

			const uint32_t sampleRate = std::max(streamSampleRate.load(), 1U);
			outputAudio(left, right, frames, sampleRate, channels);

			const uint64_t sleepNs = util_mul_div64((uint32_t)frames, kNsPerSec, sampleRate);
			os_sleepto_ns(os_gettime_ns() + sleepNs);
		}
	}

	void outputAudio(const std::vector<float> &left, const std::vector<float> &right, int frames,
			 uint32_t sampleRate, int channels)
	{
		if (!active.load() || !source || frames <= 0)
			return;

		obs_source_audio audio = {};
		audio.frames = (uint32_t)frames;
		audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
		audio.samples_per_sec = sampleRate;
		audio.speakers = speaker_layout_for_channels(channels);
		audio.data[0] = reinterpret_cast<const uint8_t *>(left.data());
		if (channels == 2)
			audio.data[1] = reinterpret_cast<const uint8_t *>(right.data());

		audio.timestamp = audio_timestamp(audio.frames, sampleRate);
		if (!firstTimestamp)
			firstTimestamp = audio.timestamp + kStartupTimeoutNs;
		if (audio.timestamp > firstTimestamp)
			obs_source_output_audio(source, &audio);
	}

	obs_source_t *source = nullptr;
	std::shared_ptr<LinkAudioManager> manager;
	std::unique_ptr<ableton::LinkAudioSource> sourceSubscription;
	std::unique_ptr<AudioRingBuffer> ringL;
	std::unique_ptr<AudioRingBuffer> ringR;

	std::mutex settingsMutex;
	std::mutex lifetimeMutex;
	std::condition_variable workerCv;
	std::mutex workerCvMutex;

	std::thread workerThread;
	std::thread audioThread;
	std::atomic<bool> workerStop{false};
	std::atomic<bool> audioThreadStop{false};
	std::atomic<bool> stateDirty{true};
	std::atomic<bool> active{false};
	bool running = false;

	std::string localPeerName = "OBS";
	std::string sourcePeerName = "Live";
	std::string sourceChannelName = "Main";
	std::string subscribedPeer;
	std::string subscribedChannel;
	int channelCount = 2;
	int bufferSize = 256;

	std::atomic<uint32_t> streamSampleRate{obs_audio_sample_rate()};
	std::atomic<uint32_t> streamChannelCount{2};
	std::atomic<uint64_t> framesReceived{0};
	std::atomic<uint64_t> framesDropped{0};
	std::atomic<int> peerCount{0};
	uint64_t firstTimestamp = 0;
};

static const char *link_audio_get_name(void *)
{
	return obs_module_text("AbletonLinkAudioSource");
}

static void link_audio_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, kLocalPeerName, "OBS");
	obs_data_set_default_string(settings, kSourcePeerName, "Live");
	obs_data_set_default_string(settings, kSourceChannelName, "Main");
	obs_data_set_default_int(settings, kChannelCount, 2);
	obs_data_set_default_int(settings, kBufferSize, 256);
}

static obs_properties_t *link_audio_get_properties(void *)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, kLocalPeerName, obs_module_text("LocalPeerName"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, kSourcePeerName, obs_module_text("SourcePeerName"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, kSourceChannelName, obs_module_text("SourceChannelName"), OBS_TEXT_DEFAULT);

	obs_property_t *channels = obs_properties_add_list(props, kChannelCount, obs_module_text("Channels"),
							   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(channels, obs_module_text("Mono"), 1);
	obs_property_list_add_int(channels, obs_module_text("Stereo"), 2);

	obs_property_t *buffer = obs_properties_add_int(props, kBufferSize, obs_module_text("BufferSize"), 64, 4096,
							64);
	obs_property_int_set_suffix(buffer, " frames");

	return props;
}

static void *link_audio_create(obs_data_t *settings, obs_source_t *source)
{
	return new LinkAudioSource(settings, source);
}

static void link_audio_destroy(void *data)
{
	delete static_cast<LinkAudioSource *>(data);
}

static void link_audio_update(void *data, obs_data_t *settings)
{
	static_cast<LinkAudioSource *>(data)->update(settings);
}

static void link_audio_activate(void *data)
{
	static_cast<LinkAudioSource *>(data)->activate();
}

static void link_audio_deactivate(void *data)
{
	static_cast<LinkAudioSource *>(data)->deactivate();
}

} // namespace

void RegisterLinkAudioSource()
{
	obs_source_info info = {};
	info.id = "ableton_link_audio_source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_DO_NOT_SELF_MONITOR;
	info.get_name = link_audio_get_name;
	info.create = link_audio_create;
	info.destroy = link_audio_destroy;
	info.update = link_audio_update;
	info.activate = link_audio_activate;
	info.deactivate = link_audio_deactivate;
	info.get_defaults = link_audio_get_defaults;
	info.get_properties = link_audio_get_properties;
	info.icon_type = OBS_ICON_TYPE_AUDIO_INPUT;
	obs_register_source(&info);
}