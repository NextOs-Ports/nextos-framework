/*
 * Host playback backend for Pikmin's HVQM4 movies.
 *
 * The original Jac_HVQM implementation is coupled to the GameCube AI/DSP and
 * performs 32-bit pointer DMA through JAudio. This keeps the game's native
 * MovSample flow and public Jac_StreamMovie* seam, but supplies the two pieces
 * the host needs: bounded DVD reads into the original CPU decoder and an SDL3
 * PCM sink. The HVQM4 decoder itself remains the projectPiki implementation
 * (see src/hvqm4dec/hvqm4dec.c for its attribution).
 */

#include "jaudio/app_inter.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <time.h>

#include <dolphin/dvd.h>

#include "hvqm4.h"
#include "port/audio_sink.h"
#include "port/jaudio_state.h"
#include "port/os_port.h"

namespace {

constexpr size_t kDVDCacheSize    = 0x80000;
constexpr size_t kRecordBufferSize = 0x40000;
constexpr size_t kPcmScratchFrames = 4096;
constexpr size_t kMaxPictures      = 24;
constexpr uint32_t kMovieFrameRate = 30;
constexpr uint64_t kAudioStallNs    = 1500000000ULL;

struct Picture {
	u8* data;
	u32 frame;
	u8 state; // 0=free, 1=decoded/ready, 2=currently displayed
};

struct WorkArena {
	u8* cursor;
	size_t remaining;

	void* take(size_t size, size_t alignment)
	{
		const uintptr_t current = reinterpret_cast<uintptr_t>(cursor);
		const uintptr_t aligned = (current + alignment - 1) & ~(alignment - 1);
		const size_t padding    = aligned - current;
		if (padding > remaining || size > remaining - padding) {
			return nullptr;
		}
		cursor = reinterpret_cast<u8*>(aligned + size);
		remaining -= padding + size;
		return reinterpret_cast<void*>(aligned);
	}
};

struct MovieState {
	DVDFileInfo file;
	bool fileOpen;
	bool initialized;
	bool failed;
	bool decodeFinished;
	bool playbackStarted;
	bool audioSinkReady;
	bool audioInputFlushed;

	u8* dvdCache;
	u32 dvdCacheBase;
	u32 dvdCacheValid;
	u8* recordBuffer;
	s16* pcmScratch;
	SeqObj* sequence;

	Picture pictures[kMaxPictures];
	u32 pictureCount;
	s32 displayedPicture;
	void* reference1;
	void* reference2;

	u32 fileSize;
	u32 headerSize;
	u32 gopCount;
	u32 gopIndex;
	u32 gopEnd;
	u32 gopFrameCount;
	u32 gopBaseFrame;
	u32 streamOffset;
	u32 totalFrames;
	u32 decodedFrames;
	u16 width;
	u16 height;
	u8 horizontalSampling;
	u8 verticalSampling;
	u8 audioFormat;
	u32 audioRate;
	u32 audioBytes;
	u32 audioBytesSeen;
	u8 audioCarry[36];
	u32 audioCarrySize;
	s16 adpcmStateA[4];
	s16 adpcmStateB[4];
	u64 playbackStartNs;
	u64 lastAudioProgressFrames;
	u64 lastAudioProgressNs;
	char path[160];
};

MovieState sMovie;
std::atomic<bool> sMovieAudioActive { false };

static void closeMovieAudioSink()
{
	const bool owned = sMovieAudioActive.exchange(false, std::memory_order_acq_rel);
	if (owned || sMovie.audioSinkReady) {
		PikiAudioSinkClose();
	}
	sMovie.audioSinkReady = false;
}

static u16 readBE16(const void* data)
{
	const u8* p = static_cast<const u8*>(data);
	return static_cast<u16>((static_cast<u16>(p[0]) << 8) | p[1]);
}

static u32 readBE32(const void* data)
{
	const u8* p = static_cast<const u8*>(data);
	return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) | (static_cast<u32>(p[2]) << 8)
	     | static_cast<u32>(p[3]);
}

static u64 monotonicNs()
{
	timespec now {};
	clock_gettime(CLOCK_MONOTONIC, &now);
	return static_cast<u64>(now.tv_sec) * 1000000000ULL + static_cast<u64>(now.tv_nsec);
}

static void cooperativePause(long nanoseconds)
{
	const timespec delay = { 0, nanoseconds };
	OSPortBlockingEnter();
	nanosleep(&delay, nullptr);
	OSPortBlockingLeave();
}

static void failMovie(const char* reason)
{
	if (!sMovie.failed) {
		std::fprintf(stderr, "[hvqm] %s: %s\n", reason, sMovie.path[0] != '\0' ? sMovie.path : "(no movie)");
	}
	sMovie.failed = true;
}

static bool loadDVDCache(u32 offset)
{
	const u32 base = offset & ~static_cast<u32>(kDVDCacheSize - 1);
	if (base >= sMovie.fileSize) {
		return false;
	}

	const u32 available = sMovie.fileSize - base;
	const u32 wanted    = std::min<u32>(available, kDVDCacheSize);
	const u32 transfer  = (wanted + 31u) & ~31u;

	OSPortBlockingEnter();
	const s32 result = DVDReadPrio(&sMovie.file, sMovie.dvdCache, static_cast<s32>(transfer), static_cast<s32>(base), 2);
	OSPortBlockingLeave();
	if (result < static_cast<s32>(wanted)) {
		failMovie("DVD read failed");
		return false;
	}

	sMovie.dvdCacheBase  = base;
	sMovie.dvdCacheValid = wanted;
	return true;
}

static bool readMovie(u32 offset, void* destination, size_t size)
{
	if (size > sMovie.fileSize || offset > sMovie.fileSize - size) {
		failMovie("record exceeds movie bounds");
		return false;
	}

	u8* out = static_cast<u8*>(destination);
	while (size != 0) {
		if (offset < sMovie.dvdCacheBase || offset >= sMovie.dvdCacheBase + sMovie.dvdCacheValid) {
			if (!loadDVDCache(offset)) {
				return false;
			}
		}
		const u32 cacheOffset = offset - sMovie.dvdCacheBase;
		const size_t chunk    = std::min<size_t>(size, sMovie.dvdCacheValid - cacheOffset);
		std::memcpy(out, sMovie.dvdCache + cacheOffset, chunk);
		out += chunk;
		offset += static_cast<u32>(chunk);
		size -= chunk;
	}
	return true;
}

static bool openMovieFile(const char* requested)
{
	const char* basename = std::strrchr(requested, '/');
	basename             = basename != nullptr ? basename + 1 : requested;

	const char* candidates[3];
	char absolutePath[160];
	char relativePath[160];
	std::snprintf(absolutePath, sizeof(absolutePath), "/dataDir/MovieData/%s", basename);
	std::snprintf(relativePath, sizeof(relativePath), "dataDir/MovieData/%s", basename);
	candidates[0] = absolutePath;
	candidates[1] = relativePath;
	candidates[2] = requested;

	for (const char* candidate : candidates) {
		if (DVDOpen(candidate, &sMovie.file)) {
			sMovie.fileOpen = true;
			sMovie.fileSize = sMovie.file.length;
			std::snprintf(sMovie.path, sizeof(sMovie.path), "%s", candidate);
			return true;
		}
	}
	std::snprintf(sMovie.path, sizeof(sMovie.path), "%s", requested);
	failMovie("movie not found on disc");
	return false;
}

static s16 clamp16(s32 value)
{
	if (value > 0x7fff) {
		return 0x7fff;
	}
	if (value < -0x8000) {
		return -0x8000;
	}
	return static_cast<s16>(value);
}

static void applyOutputMode(s32& left, s32& right)
{
	if (!PikiJAudioStereoOutput()) {
		constexpr s64 monoMix = 0xBFFD;
		const s32 mono = static_cast<s32>((left * monoMix) / 0x10000 + (right * monoMix) / 0x10000);
		left           = mono;
		right          = mono;
	}
}

static void decodeStereoADPCM(const u8* source, s16* left, s16* right, s16 state[4])
{
	static constexpr s16 coefficients[16][2] = {
		{ +0x0000, +0x0000 }, { +0x0800, +0x0000 }, { +0x0000, +0x0800 }, { +0x0400, +0x0400 },
		{ +0x1000, -0x0800 }, { +0x0E00, -0x0600 }, { +0x0C00, -0x0400 }, { +0x1200, -0x0A00 },
		{ +0x1068, -0x08C8 }, { +0x12C0, -0x08FC }, { +0x1400, -0x0C00 }, { +0x0800, -0x0800 },
		{ +0x0400, -0x0400 }, { -0x0400, +0x0400 }, { -0x0400, +0x0000 }, { -0x0800, +0x0000 },
	};

	s16 latestLeft      = state[0];
	s16 previousLeft    = state[1];
	s16 latestRight     = state[2];
	s16 previousRight   = state[3];

	for (int channel = 0; channel < 2; ++channel) {
		const u8 header     = *source++;
		const int scale     = header >> 4;
		const s16 coef1     = coefficients[header & 0xf][0];
		const s16 coef2     = coefficients[header & 0xf][1];
		s16& latest         = channel == 0 ? latestLeft : latestRight;
		s16& previous       = channel == 0 ? previousLeft : previousRight;
		s16* destination    = channel == 0 ? left : right;

		for (int pair = 0; pair < 8; ++pair) {
			const u8 packed = *source++;
			for (int half = 0; half < 2; ++half) {
				const u8 nibble = half == 0 ? packed >> 4 : packed & 0xf;
				const s32 signedNibble = nibble < 8 ? nibble : static_cast<s32>(nibble) - 16;
				const s32 prediction   = (coef1 * latest + coef2 * previous) >> 11;
				const s16 sample       = clamp16(signedNibble * (1 << scale) + prediction);
				previous               = latest;
				latest                 = sample;
				*destination++         = sample;
			}
		}
	}

	state[0] = latestLeft;
	state[1] = previousLeft;
	state[2] = latestRight;
	state[3] = previousRight;
}

static void decodeCompressedAudioBlock(const u8* block, s16* output)
{
	s16 leftA[16];
	s16 rightA[16];
	s16 leftB[16];
	s16 rightB[16];

	decodeStereoADPCM(block, leftA, rightA, sMovie.adpcmStateA);
	if (sMovie.audioFormat == 5) {
		decodeStereoADPCM(block + 18, leftB, rightB, sMovie.adpcmStateB);
	}

	const s32 bgmLevel = PikiJAudioBGMStreamLevel();
	const s32 seLevel  = sMovie.audioFormat == 5 ? PikiJAudioSEStreamLevel() : 0;
	for (size_t i = 0; i < 16; ++i) {
		s32 left  = (leftA[i] * bgmLevel) >> 15;
		s32 right = (rightA[i] * bgmLevel) >> 15;
		if (sMovie.audioFormat == 5) {
			left += (leftB[i] * seLevel) >> 15;
			right += (rightB[i] * seLevel) >> 15;
		}
		applyOutputMode(left, right);
		output[i * 2]     = clamp16(left);
		output[i * 2 + 1] = clamp16(right);
	}
}

static bool consumeCompressedAudio(const u8* data, size_t size)
{
	const size_t blockSize = sMovie.audioFormat == 5 ? 36 : 18;
	size_t pendingFrames   = 0;
	auto appendBlock       = [&](const u8* block) {
		decodeCompressedAudioBlock(block, sMovie.pcmScratch + pendingFrames * 2);
		pendingFrames += 16;
		if (pendingFrames == kPcmScratchFrames) {
			if (sMovie.audioSinkReady && !PikiAudioSinkQueue(sMovie.pcmScratch, pendingFrames)) {
				std::fprintf(stderr, "[hvqm] audio device lost; continuing with silent movie clock\n");
				closeMovieAudioSink();
			}
			pendingFrames = 0;
		}
		return true;
	};

	if (sMovie.audioCarrySize != 0) {
		const size_t needed = blockSize - sMovie.audioCarrySize;
		const size_t copy   = std::min(needed, size);
		std::memcpy(sMovie.audioCarry + sMovie.audioCarrySize, data, copy);
		sMovie.audioCarrySize += static_cast<u32>(copy);
		data += copy;
		size -= copy;
		if (sMovie.audioCarrySize == blockSize) {
			if (!appendBlock(sMovie.audioCarry)) {
				return false;
			}
			sMovie.audioCarrySize = 0;
		}
	}

	while (size >= blockSize) {
		if (!appendBlock(data)) {
			return false;
		}
		data += blockSize;
		size -= blockSize;
	}
	if (size != 0) {
		std::memcpy(sMovie.audioCarry, data, size);
		sMovie.audioCarrySize = static_cast<u32>(size);
	}
	if (pendingFrames != 0 && sMovie.audioSinkReady && !PikiAudioSinkQueue(sMovie.pcmScratch, pendingFrames)) {
		std::fprintf(stderr, "[hvqm] audio device lost; continuing with silent movie clock\n");
		closeMovieAudioSink();
	}
	return true;
}

static bool queuePCMFrames(const u8* data, size_t frames)
{
	const s32 bgmLevel = PikiJAudioBGMStreamLevel();
	for (size_t i = 0; i < frames; ++i) {
		s32 left;
		s32 right;
		if (sMovie.audioFormat == 2) {
			left  = static_cast<s16>(readBE16(data + i * 4));
			right = static_cast<s16>(readBE16(data + i * 4 + 2));
		} else {
			left  = (static_cast<int>(data[i * 2]) - 128) * 256;
			right = (static_cast<int>(data[i * 2 + 1]) - 128) * 256;
		}
		left  = (left * bgmLevel) >> 15;
		right = (right * bgmLevel) >> 15;
		applyOutputMode(left, right);
		sMovie.pcmScratch[i * 2]     = clamp16(left);
		sMovie.pcmScratch[i * 2 + 1] = clamp16(right);
	}
	if (sMovie.audioSinkReady && !PikiAudioSinkQueue(sMovie.pcmScratch, frames)) {
		std::fprintf(stderr, "[hvqm] audio device lost; continuing with silent movie clock\n");
		closeMovieAudioSink();
	}
	return true;
}

static bool consumePCMAudio(const u8* data, size_t size)
{
	const size_t frameSize = sMovie.audioFormat == 2 ? 4 : 2;
	if (sMovie.audioCarrySize != 0) {
		const size_t needed = frameSize - sMovie.audioCarrySize;
		const size_t copy   = std::min(needed, size);
		std::memcpy(sMovie.audioCarry + sMovie.audioCarrySize, data, copy);
		sMovie.audioCarrySize += static_cast<u32>(copy);
		data += copy;
		size -= copy;
		if (sMovie.audioCarrySize == frameSize) {
			if (!queuePCMFrames(sMovie.audioCarry, 1)) {
				return false;
			}
			sMovie.audioCarrySize = 0;
		}
	}

	while (size >= frameSize) {
		const size_t frames = std::min<size_t>(size / frameSize, kPcmScratchFrames);
		if (!queuePCMFrames(data, frames)) {
			return false;
		}
		const size_t consumed = frames * frameSize;
		data += consumed;
		size -= consumed;
	}
	if (size != 0) {
		std::memcpy(sMovie.audioCarry, data, size);
		sMovie.audioCarrySize = static_cast<u32>(size);
	}
	return true;
}

static bool consumeAudio(const u8* data, size_t size)
{
	if (size > sMovie.audioBytes - sMovie.audioBytesSeen) {
		failMovie("audio exceeds header size");
		return false;
	}
	sMovie.audioBytesSeen += static_cast<u32>(size);

	switch (sMovie.audioFormat) {
	case 4:
	case 5:
		return consumeCompressedAudio(data, size);
	case 2:
	case 3:
		return consumePCMAudio(data, size);
	default:
		failMovie("unsupported movie audio format");
		return false;
	}
}

static void maybeFlushAudioInput()
{
	if (!sMovie.audioSinkReady || sMovie.audioInputFlushed || sMovie.audioBytesSeen != sMovie.audioBytes
	    || sMovie.audioCarrySize != 0) {
		return;
	}
	if (!PikiAudioSinkFlush()) {
		std::fprintf(stderr, "[hvqm] audio flush failed; continuing with silent movie clock\n");
		closeMovieAudioSink();
		return;
	}
	sMovie.audioInputFlushed = true;
	std::fprintf(stderr, "[hvqm] complete audio stream queued; draining device\n");
}

static bool beginNextGop()
{
	if (sMovie.gopIndex == sMovie.gopCount) {
		sMovie.decodeFinished = true;
		if (sMovie.audioCarrySize != 0 || sMovie.audioBytesSeen != sMovie.audioBytes) {
			failMovie("incomplete audio stream at end of movie");
			return false;
		}
		return false;
	}

	u8 header[20];
	if (!readMovie(sMovie.streamOffset, header, sizeof(header))) {
		return false;
	}
	const u32 payloadSize = readBE32(header + 4);
	sMovie.gopFrameCount  = readBE32(header + 8);
	sMovie.streamOffset += sizeof(header);
	if (payloadSize > sMovie.fileSize - sMovie.streamOffset) {
		failMovie("GOP exceeds movie bounds");
		return false;
	}
	sMovie.gopEnd = sMovie.streamOffset + payloadSize;
	return true;
}

static bool pictureSlotAvailable(u32 frame)
{
	const Picture& picture = sMovie.pictures[frame % sMovie.pictureCount];
	return picture.state == 0;
}

static bool decodeVideoRecord(u16 flags, u32 recordSize)
{
	if (recordSize < 4 || recordSize > kRecordBufferSize) {
		failMovie("invalid video record size");
		return false;
	}
	u8 frameBytes[4];
	if (!readMovie(sMovie.streamOffset + 8, frameBytes, sizeof(frameBytes))) {
		return false;
	}
	const u32 localFrame = readBE32(frameBytes);
	if (localFrame >= sMovie.gopFrameCount) {
		failMovie("video frame outside GOP");
		return false;
	}
	const u32 frame = sMovie.gopBaseFrame + localFrame;
	if (frame >= sMovie.totalFrames || !pictureSlotAvailable(frame)) {
		return false;
	}
	if (!readMovie(sMovie.streamOffset + 8, sMovie.recordBuffer, recordSize)) {
		return false;
	}

	Picture& picture = sMovie.pictures[frame % sMovie.pictureCount];
	u8* code         = sMovie.recordBuffer + 4;
	switch (flags & 0xff) {
	case 0x10:
		HVQM4DecodeIpic(sMovie.sequence, code, picture.data);
		sMovie.reference2 = sMovie.reference1;
		sMovie.reference1 = picture.data;
		break;
	case 0x20:
		if (sMovie.reference1 == nullptr) {
			failMovie("P frame has no reference");
			return false;
		}
		HVQM4DecodePpic(sMovie.sequence, code, picture.data, sMovie.reference1);
		sMovie.reference2 = sMovie.reference1;
		sMovie.reference1 = picture.data;
		break;
	case 0x30:
		if (sMovie.reference1 == nullptr || sMovie.reference2 == nullptr) {
			failMovie("B frame has no references");
			return false;
		}
		HVQM4DecodeBpic(sMovie.sequence, code, picture.data, sMovie.reference2, sMovie.reference1);
		break;
	default:
		failMovie("unsupported video frame type");
		return false;
	}

	picture.frame = frame;
	picture.state = 1;
	sMovie.decodedFrames++;
	return true;
}

static bool processRecord()
{
	if (sMovie.streamOffset == sMovie.gopEnd) {
		sMovie.gopBaseFrame += sMovie.gopFrameCount;
		sMovie.gopIndex++;
		return beginNextGop();
	}
	if (sMovie.streamOffset > sMovie.gopEnd || sMovie.gopEnd - sMovie.streamOffset < 8) {
		failMovie("truncated GOP record header");
		return false;
	}

	u8 header[8];
	if (!readMovie(sMovie.streamOffset, header, sizeof(header))) {
		return false;
	}
	const u16 recordType = readBE16(header);
	const u16 flags      = readBE16(header + 2);
	const u32 recordSize = readBE32(header + 4);
	if (recordSize > sMovie.gopEnd - sMovie.streamOffset - 8 || recordSize > kRecordBufferSize) {
		failMovie("invalid movie record size");
		return false;
	}

	if (recordType == 0) {
		if (!readMovie(sMovie.streamOffset + 8, sMovie.recordBuffer, recordSize)
		    || !consumeAudio(sMovie.recordBuffer, recordSize)) {
			return false;
		}
		maybeFlushAudioInput();
	} else if (recordType == 1) {
		if (!decodeVideoRecord(flags, recordSize)) {
			return false;
		}
	} else {
		failMovie("unknown movie record type");
		return false;
	}

	sMovie.streamOffset += 8 + recordSize;
	return true;
}

static void maybeStartPlayback()
{
	if (sMovie.playbackStarted || sMovie.failed) {
		return;
	}
	const u32 prebuffer = std::min<u32>(8, sMovie.pictureCount);
	if (sMovie.decodedFrames < prebuffer && !sMovie.decodeFinished) {
		return;
	}
	if (sMovie.audioSinkReady && !PikiAudioSinkResume()) {
		closeMovieAudioSink();
	}
	sMovie.playbackStartNs = monotonicNs();
	sMovie.lastAudioProgressFrames = PikiAudioSinkPlayedFrames();
	sMovie.lastAudioProgressNs     = sMovie.playbackStartNs;
	sMovie.playbackStarted = true;
	std::fprintf(stderr, "[hvqm] playback started after %u buffered frames%s\n", sMovie.decodedFrames,
	             sMovie.audioSinkReady ? " with audio" : " (silent clock fallback)");
}

static u64 targetFrame()
{
	if (!sMovie.playbackStarted) {
		return 0;
	}
	const u64 elapsedNs = monotonicNs() - sMovie.playbackStartNs;
	u64 target          = elapsedNs * kMovieFrameRate / 1000000000ULL;
	if (sMovie.audioSinkReady && sMovie.audioRate != 0) {
		u64 submitted;
		u64 queued;
		if (!PikiAudioSinkGetProgress(&submitted, &queued)) {
			std::fprintf(stderr, "[hvqm] audio progress query failed; continuing with silent movie clock\n");
			closeMovieAudioSink();
		} else {
			const u64 played = queued < submitted ? submitted - queued : 0;
			const u64 now    = monotonicNs();
			if (played != sMovie.lastAudioProgressFrames) {
				sMovie.lastAudioProgressFrames = played;
				sMovie.lastAudioProgressNs     = now;
			} else if (now - sMovie.lastAudioProgressNs >= kAudioStallNs) {
				std::fprintf(stderr, "[hvqm] audio device made no progress for 1.5 s; continuing with silent movie clock\n");
				closeMovieAudioSink();
			}
			if (sMovie.audioSinkReady) {
				const u64 audioTarget = played * kMovieFrameRate / sMovie.audioRate;
				target                = std::min(target, audioTarget + 2);
			}
		}
	}
	return target;
}

static void closeMovie()
{
	closeMovieAudioSink();
	if (sMovie.fileOpen) {
		DVDClose(&sMovie.file);
		sMovie.fileOpen = false;
	}
	sMovie.initialized = false;
}

} // namespace

extern "C" {

int PikiMovieAudioActive(void)
{
	return sMovieAudioActive.load(std::memory_order_acquire) ? 1 : 0;
}

void Jac_StreamMovieInit(immut char* filepath, u8* movieWorkBuffer, int movieWorkSize)
{
	closeMovie();
	std::memset(&sMovie, 0, sizeof(sMovie));
	sMovie.displayedPicture = -1;

	if (filepath == nullptr || movieWorkBuffer == nullptr || movieWorkSize <= 0) {
		failMovie("invalid movie initialization");
		return;
	}

	WorkArena arena { movieWorkBuffer, static_cast<size_t>(movieWorkSize) };
	sMovie.dvdCache     = static_cast<u8*>(arena.take(kDVDCacheSize, 32));
	sMovie.recordBuffer = static_cast<u8*>(arena.take(kRecordBufferSize, 32));
	sMovie.pcmScratch   = static_cast<s16*>(arena.take(kPcmScratchFrames * 2 * sizeof(s16), 32));
	if (sMovie.dvdCache == nullptr || sMovie.recordBuffer == nullptr || sMovie.pcmScratch == nullptr
	    || !openMovieFile(filepath)) {
		failMovie("movie work buffer/open failed");
		return;
	}

	u8 header[0x44];
	if (!readMovie(0, header, sizeof(header)) || std::memcmp(header, "HVQM4 1.3", 9) != 0) {
		failMovie("invalid HVQM4 header");
		return;
	}

	sMovie.headerSize         = readBE32(header + 0x10);
	sMovie.gopCount           = readBE32(header + 0x18);
	sMovie.totalFrames        = readBE32(header + 0x1c);
	sMovie.audioBytes         = readBE32(header + 0x30);
	sMovie.width              = readBE16(header + 0x34);
	sMovie.height             = readBE16(header + 0x36);
	sMovie.horizontalSampling = header[0x38];
	sMovie.verticalSampling   = header[0x39];
	sMovie.audioFormat        = header[0x3c];
	sMovie.audioRate          = readBE32(header + 0x40);

	if (sMovie.headerSize < sizeof(header) || sMovie.headerSize >= sMovie.fileSize || sMovie.gopCount == 0
	    || sMovie.totalFrames == 0 || sMovie.width == 0 || sMovie.height == 0
	    || (sMovie.horizontalSampling != 1 && sMovie.horizontalSampling != 2)
	    || (sMovie.verticalSampling != 1 && sMovie.verticalSampling != 2)
	    || (sMovie.audioFormat < 2 || sMovie.audioFormat > 5) || sMovie.audioRate == 0) {
		failMovie("unsupported HVQM4 stream description");
		return;
	}

	VideoInfo videoInfo {};
	videoInfo.width             = sMovie.width;
	videoInfo.height            = sMovie.height;
	videoInfo.h_sampling_rate   = sMovie.horizontalSampling;
	videoInfo.v_sampling_rate   = sMovie.verticalSampling;
	sMovie.sequence             = static_cast<SeqObj*>(arena.take(sizeof(SeqObj), 32));
	if (sMovie.sequence == nullptr) {
		failMovie("no memory for decoder sequence");
		return;
	}
	HVQM4InitDecoder();
	HVQM4InitSeqObj(sMovie.sequence, &videoInfo);
	const u32 decoderSize = (HVQM4BuffSize(sMovie.sequence) + 31u) & ~31u;
	void* decoderBuffer   = arena.take(decoderSize, 32);
	if (decoderBuffer == nullptr) {
		failMovie("no memory for HVQM4 decoder");
		return;
	}
	HVQM4SetBuffer(sMovie.sequence, decoderBuffer);

	const size_t ySize     = static_cast<size_t>(sMovie.width) * sMovie.height;
	const size_t chromaSize = static_cast<size_t>(sMovie.width / sMovie.horizontalSampling)
	                        * (sMovie.height / sMovie.verticalSampling);
	const size_t pictureSize = ySize + chromaSize * 2;
	while (sMovie.pictureCount < kMaxPictures) {
		u8* picture = static_cast<u8*>(arena.take(pictureSize, 32));
		if (picture == nullptr) {
			break;
		}
		sMovie.pictures[sMovie.pictureCount++].data = picture;
	}
	if (sMovie.pictureCount < 3) {
		failMovie("not enough movie picture buffers");
		return;
	}

	/*
	 * Publish ownership before PikiAudioSinkOpen replaces JAudio's singleton
	 * stream.  The mixer keeps advancing its native state but stops touching
	 * the sink until closeMovieAudioSink drops this flag.
	 */
	sMovieAudioActive.store(true, std::memory_order_release);
	sMovie.audioSinkReady = PikiAudioSinkOpen(static_cast<int>(sMovie.audioRate)) != 0;
	if (!sMovie.audioSinkReady) {
		sMovieAudioActive.store(false, std::memory_order_release);
	}
	sMovie.streamOffset   = sMovie.headerSize;
	sMovie.initialized    = true;
	if (!beginNextGop()) {
		failMovie("could not read first GOP");
		return;
	}
	std::fprintf(stderr, "[hvqm] %s: %ux%u, %u frames, %u Hz format %u, %u picture buffers\n", sMovie.path,
	             sMovie.width, sMovie.height, sMovie.totalFrames, sMovie.audioRate, sMovie.audioFormat,
	             sMovie.pictureCount);
}

void Jac_StreamMovieUpdate(void)
{
	if (!sMovie.initialized || sMovie.failed || sMovie.decodeFinished) {
		cooperativePause(2000000L);
		return;
	}

	bool advanced = false;
	for (int record = 0; record < 2 && !sMovie.failed && !sMovie.decodeFinished; ++record) {
		const u32 before = sMovie.streamOffset;
		if (!processRecord()) {
			break;
		}
		advanced |= before != sMovie.streamOffset;
	}
	maybeStartPlayback();
	cooperativePause(advanced ? 1000000L : 2000000L);
}

void Jac_StreamMovieStop(void)
{
	if (sMovie.initialized || sMovie.fileOpen) {
		std::fprintf(stderr, "[hvqm] stopped at decoded frame %u/%u\n", sMovie.decodedFrames, sMovie.totalFrames);
	}
	closeMovie();
}

int Jac_StreamMovieGetPicture(void* pictureBuffer, int* widthOut, int* heightOut)
{
	if (pictureBuffer == nullptr || widthOut == nullptr || heightOut == nullptr) {
		return -1;
	}
	*static_cast<void**>(pictureBuffer) = nullptr;
	*widthOut                          = sMovie.width;
	*heightOut                         = sMovie.height;
	if (!sMovie.initialized || sMovie.failed) {
		return -1;
	}

	maybeStartPlayback();
	if (!sMovie.playbackStarted) {
		return 0;
	}
	if (sMovie.audioSinkReady && sMovie.audioInputFlushed) {
		const int drained = PikiAudioSinkDrained();
		if (drained < 0) {
			std::fprintf(stderr, "[hvqm] audio drain query failed; continuing with silent movie clock\n");
			closeMovieAudioSink();
		} else {
			const u64 submittedFrames = PikiAudioSinkSubmittedFrames();
			const u64 nominalAudioNs  = submittedFrames * 1000000000ULL / sMovie.audioRate;
			const bool nominalEndReached = monotonicNs() - sMovie.playbackStartNs >= nominalAudioNs;
			if (drained > 0 && nominalEndReached) {
				const u64 finalFrame = targetFrame();
				std::fprintf(stderr, "[hvqm] audio device drained at video frame %llu/%u\n",
				             static_cast<unsigned long long>(finalFrame), sMovie.totalFrames);
				return -1;
			}
		}
	}

	const u64 wanted = targetFrame();
	s32 best         = -1;
	u32 bestFrame    = 0;
	for (u32 i = 0; i < sMovie.pictureCount; ++i) {
		const Picture& picture = sMovie.pictures[i];
		if (picture.state != 0 && picture.frame <= wanted && (best == -1 || picture.frame > bestFrame)) {
			best      = static_cast<s32>(i);
			bestFrame = picture.frame;
		}
	}

	bool pictureChanged = false;
	if (best != -1 && best != sMovie.displayedPicture) {
		for (u32 i = 0; i < sMovie.pictureCount; ++i) {
			if (static_cast<s32>(i) != best && sMovie.pictures[i].state != 0
			    && sMovie.pictures[i].frame <= bestFrame) {
				sMovie.pictures[i].state = 0;
			}
		}
		sMovie.pictures[best].state = 2;
		sMovie.displayedPicture     = best;
		pictureChanged              = true;
	}

	if (sMovie.displayedPicture >= 0) {
		Picture& displayed = sMovie.pictures[sMovie.displayedPicture];
		if (pictureChanged) {
			*static_cast<void**>(pictureBuffer) = displayed.data;
			return static_cast<int>(displayed.frame + 1);
		}
		if (!sMovie.audioSinkReady && sMovie.decodeFinished && wanted >= sMovie.totalFrames) {
			return -1;
		}
		return 0;
	}

	if (!sMovie.audioSinkReady && sMovie.decodeFinished && wanted >= sMovie.totalFrames) {
		return -1;
	}
	return 0;
}

} // extern "C"
