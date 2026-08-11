#include "CoreAudioSink.h"

#include <cmath>
#include <cstring>

namespace {
const size_t kFramesPerBuffer = 2048;
}

CoreAudioSink::CoreAudioSink()
	:
	fQueue(NULL),
	fSampleRate(0),
	fBatched(false),
	fInputChannels(1),
	fVolume(0.7f),
	fReadPos(0),
	fWritePos(0),
	fFill(0),
	fPrimeFrames(0),
	fPriming(true),
	fReadFrac(0.0),
	fRatio(1.0),
	fPeak(0.0f),
	fUnderruns(0),
	fOverruns(0),
	fHaveMediaPts(false),
	fReadPts90k(0.0)
{
	pthread_mutex_init(&fLock, NULL);
	for (int i = 0; i < kBuffers; i++)
		fBuffers[i] = NULL;
}

CoreAudioSink::~CoreAudioSink()
{
	Shutdown();
	pthread_mutex_destroy(&fLock);
}

status_t
CoreAudioSink::Configure(uint32 sampleRate, bool batched, uint32 inputChannels)
{
	if (sampleRate == 0 || (inputChannels != 1 && inputChannels != 2))
		return B_BAD_VALUE;
	if (fQueue != NULL && sampleRate == fSampleRate && batched == fBatched
			&& inputChannels == fInputChannels)
		return B_OK;
	Shutdown();

	AudioStreamBasicDescription format;
	memset(&format, 0, sizeof(format));
	format.mSampleRate = (Float64)sampleRate;
	format.mFormatID = kAudioFormatLinearPCM;
	format.mFormatFlags = kAudioFormatFlagIsSignedInteger
		| kAudioFormatFlagIsPacked;
	format.mChannelsPerFrame = 2;
	format.mBitsPerChannel = 16;
	format.mFramesPerPacket = 1;
	format.mBytesPerFrame = 4;
	format.mBytesPerPacket = 4;

	if (AudioQueueNewOutput(&format, &CoreAudioSink::_Callback, this, NULL,
			NULL, 0, &fQueue) != noErr) {
		fQueue = NULL;
		return B_ERROR;
	}

	// Analog samples arrive continuously, but DMB is decoded from a 1.536 s USB
	// block and therefore arrives in bursts. A one-second ring silently dropped
	// the oldest third of every DMB block. Keep four seconds for that path and
	// wait for one block before starting; this is buffering, not extra latency,
	// because the video cannot be decoded before that block arrives either.
	pthread_mutex_lock(&fLock);
	fSampleRate = sampleRate;
	fBatched = batched;
	fInputChannels = inputChannels;
	fRing.assign((batched ? sampleRate * 4 : sampleRate) * inputChannels, 0.0f);
	fReadPos = fWritePos = fFill = 0;
	// A DMB service's PES packing varies by more than half a second per USB
	// block even though its long-term rate is exact. The first two decoded
	// blocks naturally settle near 2.5 seconds. Steering that sawtooth toward a
	// 1.5-second target modulated playback speed every 1.536 seconds and sounded
	// like adhesive tape tearing despite zero under/overruns.
	fPrimeFrames = batched ? sampleRate * 5 / 2 : sampleRate / 4;
	fPriming = true;
	fReadFrac = 0.0;
	fRatio = 1.0;
	fUnderruns = 0;
	fOverruns = 0;
	fHaveMediaPts = false;
	fReadPts90k = 0.0;
	pthread_mutex_unlock(&fLock);

	for (int i = 0; i < kBuffers; i++) {
		if (AudioQueueAllocateBuffer(fQueue, kFramesPerBuffer * 4,
				&fBuffers[i]) != noErr) {
			return B_ERROR;
		}
		fBuffers[i]->mAudioDataByteSize = kFramesPerBuffer * 4;
		memset(fBuffers[i]->mAudioData, 0, kFramesPerBuffer * 4);
		AudioQueueEnqueueBuffer(fQueue, fBuffers[i], 0, NULL);
	}
	AudioQueueSetParameter(fQueue, kAudioQueueParam_Volume, 1.0f);
	AudioQueueStart(fQueue, NULL);
	return B_OK;
}

void
CoreAudioSink::Shutdown()
{
	if (fQueue == NULL)
		return;
	AudioQueueStop(fQueue, true);
	AudioQueueDispose(fQueue, true);
	fQueue = NULL;
	for (int i = 0; i < kBuffers; i++)
		fBuffers[i] = NULL;
	pthread_mutex_lock(&fLock);
	fSampleRate = 0;
	fBatched = false;
	fInputChannels = 1;
	fHaveMediaPts = false;
	pthread_mutex_unlock(&fLock);
}

void
CoreAudioSink::SetVolume(float gain)
{
	pthread_mutex_lock(&fLock);
	fVolume = gain < 0.0f ? 0.0f : (gain > 1.0f ? 1.0f : gain);
	pthread_mutex_unlock(&fLock);
}

void
CoreAudioSink::Write(const float* samples, size_t count)
{
	_Write(samples, count, 1, false, 0);
}

void
CoreAudioSink::WriteStereo(const float* samples, size_t frames)
{
	_Write(samples, frames, 2, false, 0);
}

void
CoreAudioSink::WriteTimed(const float* samples, size_t count, uint64 pts90k)
{
	_Write(samples, count, 1, true, pts90k);
}

void
CoreAudioSink::_Write(const float* samples, size_t frames, uint32 channels,
	bool timed, uint64 pts90k)
{
	pthread_mutex_lock(&fLock);
	size_t capacity = fInputChannels > 0 ? fRing.size() / fInputChannels : 0;
	if (capacity == 0 || channels != fInputChannels) {
		pthread_mutex_unlock(&fLock);
		return;
	}
	if (timed && !fHaveMediaPts) {
		// A timed write starts at the current tail. Normally the ring is empty;
		// account for an untimed prefix defensively so the read head still maps
		// to the first sample already queued.
		fReadPts90k = (double)pts90k
			- (double)fFill * 90000.0 / (double)fSampleRate;
		fHaveMediaPts = true;
	}
	float peak = 0.0f;
	for (size_t i = 0; i < frames; i++) {
		for (uint32 ch = 0; ch < channels; ch++) {
			float v = samples[i * channels + ch];
			float a = v < 0.0f ? -v : v;
			if (a > peak)
				peak = a;
			fRing[fWritePos * channels + ch] = v;
		}
		fWritePos = (fWritePos + 1) % capacity;
		if (fFill < capacity)
			fFill++;
		else {
			fReadPos = (fReadPos + 1) % capacity;	// overrun: drop the oldest
			if (fHaveMediaPts)
				fReadPts90k += 90000.0 / (double)fSampleRate;
			fOverruns++;
		}
	}
	fPeak = peak;
	pthread_mutex_unlock(&fLock);
}

bool
CoreAudioSink::PlaybackPts(uint64& pts90k) const
{
	pthread_mutex_lock(&fLock);
	if (!fHaveMediaPts || fPriming || fSampleRate == 0) {
		pthread_mutex_unlock(&fLock);
		return false;
	}
	// The callback fills one buffer while up to kBuffers-1 buffers are ahead
	// of it. Use the conservative full queue depth; VideoView's tolerance is
	// one frame, so this fixed estimate is preferable to starting video early.
	double queued = (double)kBuffers * kFramesPerBuffer * 90000.0
		/ (double)fSampleRate;
	double audible = fReadPts90k - queued;
	const double wrap = 8589934592.0; // MPEG 33-bit PTS
	while (audible < 0.0)
		audible += wrap;
	pts90k = (uint64)audible & 0x1ffffffffULL;
	pthread_mutex_unlock(&fLock);
	return true;
}

float
CoreAudioSink::PeakLevel() const
{
	pthread_mutex_lock(&fLock);
	float v = fPeak;
	pthread_mutex_unlock(&fLock);
	return v;
}

uint64
CoreAudioSink::Underruns() const
{
	pthread_mutex_lock(&fLock);
	uint64 v = fUnderruns;
	pthread_mutex_unlock(&fLock);
	return v;
}

uint64
CoreAudioSink::Overruns() const
{
	pthread_mutex_lock(&fLock);
	uint64 v = fOverruns;
	pthread_mutex_unlock(&fLock);
	return v;
}

size_t
CoreAudioSink::QueuedFrames() const
{
	pthread_mutex_lock(&fLock);
	size_t v = fFill;
	pthread_mutex_unlock(&fLock);
	return v;
}

void
CoreAudioSink::_Callback(void* cookie, AudioQueueRef queue,
	AudioQueueBufferRef buffer)
{
	CoreAudioSink* self = static_cast<CoreAudioSink*>(cookie);
	size_t frames = buffer->mAudioDataBytesCapacity / 4;
	self->_Fill((int16_t*)buffer->mAudioData, frames);
	buffer->mAudioDataByteSize = (UInt32)(frames * 4);
	AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);
}

void
CoreAudioSink::_Fill(int16_t* out, size_t frames)
{
	pthread_mutex_lock(&fLock);
	uint32 channels = fInputChannels;
	size_t capacity = channels > 0 ? fRing.size() / channels : 0;

	if (fPriming) {
		if (fFill < fPrimeFrames || capacity == 0) {
			memset(out, 0, frames * 2 * sizeof(int16_t));
			pthread_mutex_unlock(&fLock);
			return;
		}
		fPriming = false;
		fReadFrac = 0.0;
		fRatio = 1.0;
	}

	// Steer continuous analog input at the cushion depth; see the header. DMB
	// arrives as exact 1.536-second decoded bursts, so its instantaneous fill is
	// a large sawtooth rather than clock error. Following that sawtooth changed
	// pitch once per block (the reported intermittent "sok" sound). Its service
	// sample count is exact; the 2.5-second cushion absorbs sound-card ppm drift.
	if (fBatched) {
		fRatio = 1.0;
	} else {
		const double target = fPrimeFrames > 0 ? (double)fPrimeFrames : 1.0;
		double error = ((double)fFill - target) / target;
		double gain = 0.05;
		double limit = 0.05;
		double want = 1.0 + gain * error;
		if (want < 1.0 - limit)
			want = 1.0 - limit;
		if (want > 1.0 + limit)
			want = 1.0 + limit;
		fRatio += 0.05 * (want - fRatio);
	}

	float volume = fVolume;
	size_t produced = 0;
	while (produced < frames) {
		if (fFill < 2)
			break;
		size_t next = (fReadPos + 1) % capacity;
		for (uint32 outChannel = 0; outChannel < 2; outChannel++) {
			uint32 ch = channels == 1 ? 0 : outChannel;
			float a = fRing[fReadPos * channels + ch];
			float b = fRing[next * channels + ch];
			float v = (a + (float)fReadFrac * (b - a)) * volume;
			if (v > 1.0f)
				v = 1.0f;
			if (v < -1.0f)
				v = -1.0f;
			out[2 * produced + outChannel] = (int16_t)(v * 32767.0f);
		}
		produced++;

		fReadFrac += fRatio;
		if (fHaveMediaPts)
			fReadPts90k += fRatio * 90000.0 / (double)fSampleRate;
		while (fReadFrac >= 1.0 && fFill > 0) {
			fReadPos = (fReadPos + 1) % capacity;
			fFill--;
			fReadFrac -= 1.0;
		}
	}

	if (produced < frames) {
		memset(out + 2 * produced, 0,
			(frames - produced) * 2 * sizeof(int16_t));
		fUnderruns += frames - produced;
		fPriming = true;
		fHaveMediaPts = false;
	}
	pthread_mutex_unlock(&fLock);
}
