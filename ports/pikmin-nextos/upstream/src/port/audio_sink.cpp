#include "port/audio_sink.h"

#include <algorithm>
#include <cstdio>
#include <time.h>

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_init.h>

#include "port/os_port.h"

namespace {

SDL_AudioStream* sStream;
uint64_t sSubmittedFrames;
int sSampleRate;
bool sResumed;
bool sFlushed;
bool sAudioSubsystemOwned;
uint64_t sQueueEmptySinceNs;
uint64_t sDrainGraceNs;

uint64_t monotonicNs()
{
	timespec now {};
	clock_gettime(CLOCK_MONOTONIC, &now);
	return static_cast<uint64_t>(now.tv_sec) * 1000000000ULL + static_cast<uint64_t>(now.tv_nsec);
}

void retryDelay()
{
	const timespec delay = { 0, 250000000L };
	OSPortBlockingEnter();
	nanosleep(&delay, nullptr);
	OSPortBlockingLeave();
}

int openSink(int sampleRate, int attempts)
{
	PikiAudioSinkClose();

	if (sampleRate <= 0 || attempts <= 0 || !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
		std::fprintf(stderr, "[audio] SDL audio init failed: %s\n", SDL_GetError());
		return 0;
	}
	sAudioSubsystemOwned = true;

	const SDL_AudioSpec spec = { SDL_AUDIO_S16LE, 2, sampleRate };
	for (int attempt = 0; attempt < attempts && sStream == nullptr; ++attempt) {
		sStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
		if (sStream == nullptr && attempt + 1 != attempts) {
			retryDelay();
		}
	}

	if (sStream == nullptr) {
		std::fprintf(stderr, "[audio] SDL playback open failed: %s\n", SDL_GetError());
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		sAudioSubsystemOwned = false;
		return 0;
	}

	sSubmittedFrames  = 0;
	sSampleRate       = sampleRate;
	sResumed          = false;
	sFlushed          = false;
	sQueueEmptySinceNs = 0;
	sDrainGraceNs      = 100000000ULL;

	SDL_AudioSpec deviceSpec {};
	int deviceFrames = 0;
	const SDL_AudioDeviceID device = SDL_GetAudioStreamDevice(sStream);
	if (device != 0 && SDL_GetAudioDeviceFormat(device, &deviceSpec, &deviceFrames) && deviceSpec.freq > 0
	    && deviceFrames > 0) {
		const uint64_t periodNs = static_cast<uint64_t>(deviceFrames) * 1000000000ULL
		                        / static_cast<uint64_t>(deviceSpec.freq);
		sDrainGraceNs = std::clamp<uint64_t>(periodNs * 2, 50000000ULL, 250000000ULL);
	}

	std::fprintf(stderr,
	             "[audio] SDL3 sink ready: S16LE stereo %d Hz (paused for prebuffer, drain grace %llu ms)\n",
	             sampleRate, static_cast<unsigned long long>(sDrainGraceNs / 1000000ULL));
	return 1;
}

} // namespace

extern "C" {

int PikiAudioSinkOpen(int sampleRate)
{
	return openSink(sampleRate, 13);
}

int PikiAudioSinkTryOpen(int sampleRate) { return openSink(sampleRate, 1); }

int PikiAudioSinkQueue(const int16_t* samples, size_t frameCount)
{
	if (sStream == nullptr || samples == nullptr || frameCount == 0) {
		return 0;
	}
	const size_t byteCount = frameCount * 2 * sizeof(*samples);
	if (byteCount > static_cast<size_t>(INT32_MAX)
	    || !SDL_PutAudioStreamData(sStream, samples, static_cast<int>(byteCount))) {
		std::fprintf(stderr, "[audio] queue failed: %s\n", SDL_GetError());
		return 0;
	}
	sSubmittedFrames += frameCount;
	return 1;
}

int PikiAudioSinkResume(void)
{
	if (sStream == nullptr) {
		return 0;
	}
	if (!sResumed && !SDL_ResumeAudioStreamDevice(sStream)) {
		std::fprintf(stderr, "[audio] resume failed: %s\n", SDL_GetError());
		return 0;
	}
	sResumed = true;
	return 1;
}

int PikiAudioSinkFlush(void)
{
	if (sStream == nullptr) {
		return 0;
	}
	if (!sFlushed && !SDL_FlushAudioStream(sStream)) {
		std::fprintf(stderr, "[audio] flush failed: %s\n", SDL_GetError());
		return 0;
	}
	sFlushed           = true;
	sQueueEmptySinceNs = 0;
	return 1;
}

int PikiAudioSinkDrained(void)
{
	if (sStream == nullptr) {
		return 1;
	}
	if (!sFlushed) {
		return 0;
	}

	const int queuedBytes = SDL_GetAudioStreamQueued(sStream);
	if (queuedBytes < 0) {
		std::fprintf(stderr, "[audio] queued-byte query failed while draining: %s\n", SDL_GetError());
		return -1;
	}
	if (queuedBytes != 0) {
		sQueueEmptySinceNs = 0;
		return 0;
	}

	const uint64_t now = monotonicNs();
	if (sQueueEmptySinceNs == 0) {
		sQueueEmptySinceNs = now;
		return 0;
	}
	return now - sQueueEmptySinceNs >= sDrainGraceNs;
}

int PikiAudioSinkGetProgress(uint64_t* submittedFrames, uint64_t* queuedFrames)
{
	if (sStream == nullptr || submittedFrames == nullptr || queuedFrames == nullptr) {
		return 0;
	}
	const int queuedBytes = SDL_GetAudioStreamQueued(sStream);
	if (queuedBytes < 0) {
		std::fprintf(stderr, "[audio] queued-byte query failed: %s\n", SDL_GetError());
		return 0;
	}
	*submittedFrames = sSubmittedFrames;
	*queuedFrames    = static_cast<uint64_t>(queuedBytes) / (2 * sizeof(int16_t));
	return 1;
}

void PikiAudioSinkPause(void)
{
	if (sStream != nullptr && sResumed) {
		SDL_PauseAudioStreamDevice(sStream);
		sResumed = false;
	}
}

void PikiAudioSinkClose(void)
{
	if (sStream != nullptr) {
		SDL_DestroyAudioStream(sStream);
		sStream = nullptr;
	}
	if (sAudioSubsystemOwned) {
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		sAudioSubsystemOwned = false;
	}
	sSubmittedFrames = 0;
	sSampleRate       = 0;
	sResumed          = false;
	sFlushed          = false;
	sQueueEmptySinceNs = 0;
	sDrainGraceNs      = 0;
}

uint64_t PikiAudioSinkSubmittedFrames(void) { return sSubmittedFrames; }

uint64_t PikiAudioSinkQueuedFrames(void)
{
	uint64_t submitted;
	uint64_t queued;
	if (!PikiAudioSinkGetProgress(&submitted, &queued)) {
		return 0;
	}
	return queued;
}

uint64_t PikiAudioSinkPlayedFrames(void)
{
	const uint64_t queued = PikiAudioSinkQueuedFrames();
	return queued < sSubmittedFrames ? sSubmittedFrames - queued : 0;
}

} // extern "C"
