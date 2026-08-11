/*
 * CoreAudio output with the same rate control the Haiku sink needed.
 *
 * The dongle's clock and the sound card's clock are not the same, and the USB
 * path loses a little on top of that. No buffer size fixes a steady rate
 * difference - it only changes how long the ring takes to run dry, which on
 * Haiku showed up as the audio breaking up every two seconds and, when the
 * ring was doubled, every four. So the reader runs at a fractional rate
 * steered by how full the ring is; correcting a one percent shortfall means
 * playing one percent slow, which is eight cents of pitch and inaudible.
 */
#ifndef RSDR_MACOS_COREAUDIO_SINK_H
#define RSDR_MACOS_COREAUDIO_SINK_H

#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>

#include <vector>

#include "SupportDefs.h"

class CoreAudioSink {
public:
	static const int kBuffers = 4;

						CoreAudioSink();
						~CoreAudioSink();

			status_t	Configure(uint32 sampleRate, bool batched = false,
							uint32 inputChannels = 1);
			void		Shutdown();
			uint32		SampleRate() const { return fSampleRate; }

			void		Write(const float* samples, size_t count);
			void		WriteStereo(const float* samples, size_t frames);
			void		WriteTimed(const float* samples, size_t count,
							uint64 pts90k);
			void		SetVolume(float gain);

			float		PeakLevel() const;
			uint64		Underruns() const;
			uint64		Overruns() const;
			size_t		QueuedFrames() const;
	// Estimated PTS currently reaching the speakers. The AudioQueue keeps
	// several already-filled buffers ahead of the callback, which is accounted
	// for here so video is scheduled against audible rather than decoded audio.
			bool		PlaybackPts(uint64& pts90k) const;

private:
	static	void		_Callback(void* cookie, AudioQueueRef queue,
							AudioQueueBufferRef buffer);
			void		_Fill(int16_t* out, size_t frames);
			void		_Write(const float* samples, size_t frames,
							uint32 channels, bool timed, uint64 pts90k);

			AudioQueueRef		fQueue;
			AudioQueueBufferRef	fBuffers[kBuffers];
			uint32				fSampleRate;
			bool				fBatched;
			uint32				fInputChannels;
			float				fVolume;

	mutable pthread_mutex_t	fLock;
	std::vector<float>	fRing;
			size_t		fReadPos;
			size_t		fWritePos;
			size_t		fFill;
			size_t		fPrimeFrames;
			bool		fPriming;
			double		fReadFrac;
			double		fRatio;
			float		fPeak;
			uint64		fUnderruns;
			uint64		fOverruns;
			bool		fHaveMediaPts;
			double		fReadPts90k;
};

#endif // RSDR_MACOS_COREAUDIO_SINK_H
