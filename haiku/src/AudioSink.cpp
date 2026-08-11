#include "AudioSink.h"

#include <Autolock.h>
#include <SoundPlayer.h>

#include <cmath>
#include <cstring>

AudioSink::AudioSink()
	:
	fPlayer(NULL),
	fSampleRate(0),
	fInputChannels(1),
	fVolume(0.7f),
	fRingSeconds(1),
	fLock("audio ring"),
	fReadPos(0),
	fWritePos(0),
	fFill(0),
	fPrimeFrames(0),
	fPriming(true),
	fReadFrac(0.0),
	fRatio(1.0),
	fPeak(0.0f),
	fOverruns(0),
	fUnderruns(0),
	fFramesPlayed(0)
{
}

AudioSink::~AudioSink()
{
	Shutdown();
}

status_t
AudioSink::Configure(uint32 sampleRate, uint32 inputChannels)
{
	if (sampleRate == 0 || (inputChannels != 1 && inputChannels != 2))
		return B_BAD_VALUE;
	if (fPlayer != NULL && fSampleRate == sampleRate
			&& fInputChannels == inputChannels)
		return B_OK;

	Shutdown();

	media_raw_audio_format format = media_raw_audio_format::wildcard;
	format.frame_rate = (float)sampleRate;
	// Two identical channels rather than one: the mixer's mono path is the
	// less travelled one, and duplicating a float costs nothing next to the
	// filtering that produced it.
	format.channel_count = 2;
	format.format = media_raw_audio_format::B_AUDIO_FLOAT;
	format.byte_order = B_MEDIA_HOST_ENDIAN;
	// About 23 ms at 44100. Small enough that tuning feels immediate, large
	// enough that a scheduling hiccup on this CPU does not cause a dropout.
	format.buffer_size = 1024 * sizeof(float) * format.channel_count;

	// One second of slack. The demodulator delivers in 15-60 ms bursts
	// (one USB block at a time), so the ring has to hold several of those
	// plus enough margin for a GUI repaint burst on a single-core machine.
	{
		BAutolock lock(fLock);
		fInputChannels = inputChannels;
		int secs = fRingSeconds < 1 ? 1 : fRingSeconds;
		fRing.assign((size_t)sampleRate * secs * inputChannels, 0.0f);
		fReadPos = 0;
		fWritePos = 0;
		fFill = 0;
		// A quarter second prevents the repeated drain/re-prime cycle observed
		// with the old 120 ms cushion while keeping retuning responsive. A large
		// ring (the delayed player) primes with most of one second... rather,
		// with a third of the ring so chunk-sized writes have somewhere to land.
		fPrimeFrames = secs > 1 ? (size_t)sampleRate * secs / 3
			: (size_t)sampleRate * 250 / 1000;
		fPriming = true;
		fPeak = 0.0f;
	}

	BSoundPlayer* player = new BSoundPlayer(&format, "R SDR",
		&AudioSink::_PlayProc, NULL, this);
	status_t err = player->InitCheck();
	if (err != B_OK) {
		delete player;
		return err;
	}

	fPlayer = player;
	fSampleRate = sampleRate;
	fPlayer->SetVolume(fVolume);
	fPlayer->Start();
	fPlayer->SetHasData(true);
	return B_OK;
}

void
AudioSink::Shutdown()
{
	if (fPlayer != NULL) {
		// Stop() blocks until the play thread is idle, so nothing can be
		// inside _PlayProc by the time the ring buffer is touched again.
		fPlayer->Stop();
		delete fPlayer;
		fPlayer = NULL;
	}
	fSampleRate = 0;
	fInputChannels = 1;
}

void
AudioSink::SetRingSeconds(int seconds)
{
	fRingSeconds = seconds < 1 ? 1 : seconds;
}

void
AudioSink::SetVolume(float gain)
{
	if (gain < 0.0f)
		gain = 0.0f;
	if (gain > 1.0f)
		gain = 1.0f;
	fVolume = gain;
	if (fPlayer != NULL)
		fPlayer->SetVolume(gain);
}

void
AudioSink::Write(const float* samples, size_t count)
{
	_Write(samples, count, 1);
}

void
AudioSink::WriteStereo(const float* samples, size_t frames)
{
	_Write(samples, frames, 2);
}

void
AudioSink::_Write(const float* samples, size_t frames, uint32 channels)
{
	if (frames == 0)
		return;

	BAutolock lock(fLock);
	if (fRing.empty() || channels != fInputChannels)
		return;

	const size_t capacity = fRing.size() / fInputChannels;

	// Should never happen (a USB block is far shorter than half a second of
	// audio), but clamp rather than wrap past the read pointer twice.
	if (frames > capacity) {
		samples += (frames - capacity) * channels;
		frames = capacity;
	}

	size_t free = capacity - fFill;
	if (frames > free) {
		// The consumer is behind. Drop the oldest audio - discarding the
		// newest instead would leave a latency that never recovers.
		size_t drop = frames - free;
		fReadPos = (fReadPos + drop) % capacity;
		fFill -= drop;
		fOverruns += drop;
	}

	float peak = 0.0f;
	for (size_t i = 0; i < frames; i++) {
		for (uint32 ch = 0; ch < channels; ch++) {
			float v = samples[i * channels + ch];
			fRing[fWritePos * channels + ch] = v;
			float a = fabsf(v);
			if (a > peak)
				peak = a;
		}
		fWritePos = (fWritePos + 1) % capacity;
	}
	fFill += frames;

	// Decaying peak hold, so the meter does not flicker between UI frames.
	fPeak = peak > fPeak ? peak : (fPeak * 0.8f + peak * 0.2f);
}

void
AudioSink::_PlayProc(void* cookie, void* buffer, size_t size,
	const media_raw_audio_format& format)
{
	AudioSink* self = static_cast<AudioSink*>(cookie);
	size_t frames = size / (sizeof(float) * format.channel_count);
	self->_Fill(static_cast<float*>(buffer), frames);
}

void
AudioSink::_Fill(float* out, size_t frames)
{
	BAutolock lock(fLock);
	const uint32 channels = fInputChannels;
	const size_t capacity = channels > 0 ? fRing.size() / channels : 0;

	if (fPriming) {
		if (fFill < fPrimeFrames) {
			memset(out, 0, frames * 2 * sizeof(float));
			return;
		}
		fPriming = false;
		fReadFrac = 0.0;
		fRatio = 1.0;
	}

	if (capacity == 0) {
		memset(out, 0, frames * 2 * sizeof(float));
		return;
	}

	// Steer the consumption rate at the cushion depth. Deliberately slow: the
	// error is integrated over many buffers, so the ratio wanders by a
	// fraction of a percent and never steps.
	const double target = fPrimeFrames > 0 ? (double)fPrimeFrames : 1.0;
	double error = ((double)fFill - target) / target;
	double want = 1.0 + 0.05 * error;
	if (want < 0.95)
		want = 0.95;
	if (want > 1.05)
		want = 1.05;
	fRatio += 0.05 * (want - fRatio);

	size_t produced = 0;
	while (produced < frames) {
		// Linear interpolation needs the sample after the current one.
		if (fFill < 2)
			break;
		size_t next = (fReadPos + 1) % capacity;
		for (uint32 outChannel = 0; outChannel < 2; outChannel++) {
			uint32 ch = channels == 1 ? 0 : outChannel;
			float a = fRing[fReadPos * channels + ch];
			float b = fRing[next * channels + ch];
			out[2 * produced + outChannel]
				= a + (float)fReadFrac * (b - a);
		}
		produced++;

		fReadFrac += fRatio;
		while (fReadFrac >= 1.0 && fFill > 0) {
			fReadPos = (fReadPos + 1) % capacity;
			fFill--;
			fReadFrac -= 1.0;
		}
	}

	fFramesPlayed += frames;

	if (produced < frames) {
		memset(out + 2 * produced, 0, (frames - produced) * 2 * sizeof(float));
		fUnderruns += frames - produced;
		// Ran dry anyway. Rebuild the cushion rather than limping along one
		// sample away from the next gap - one longer silence is less
		// unpleasant than a rapid series of short ones.
		fPriming = true;
	}
}

float
AudioSink::PeakLevel() const
{
	BAutolock lock(fLock);
	return fPeak;
}

uint64
AudioSink::Overruns() const
{
	BAutolock lock(fLock);
	return fOverruns;
}

uint64
AudioSink::Underruns() const
{
	BAutolock lock(fLock);
	return fUnderruns;
}

uint64
AudioSink::FramesPlayed() const
{
	BAutolock lock(fLock);
	return fFramesPlayed;
}

size_t
AudioSink::QueuedFrames() const
{
	BAutolock lock(fLock);
	return fFill;
}
