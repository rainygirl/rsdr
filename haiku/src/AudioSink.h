/*
 * BSoundPlayer output with a ring buffer in front of it.
 *
 * The producer (the demodulator thread) and the consumer (BSoundPlayer's own
 * play thread) run at unrelated rates: the dongle's clock is a cheap crystal
 * with tens of ppm of error and the sound card's is a different cheap
 * crystal, so over minutes the two drift apart by whole samples no matter
 * how carefully the decimation ratios are chosen. Rather than resample, the
 * ring absorbs the difference and reports it - Overruns()/Underruns() going
 * up slowly is normal and inaudible, going up fast means the CPU is not
 * keeping up.
 */
#ifndef RSDR_AUDIO_SINK_H
#define RSDR_AUDIO_SINK_H

#include <Locker.h>
#include <MediaDefs.h>
#include <SupportDefs.h>

#include <vector>

class BSoundPlayer;

class AudioSink {
public:
							AudioSink();
							~AudioSink();

	// Tears down and recreates the player if the rate changed. Safe to call
	// repeatedly with the same rate (does nothing).
			status_t		Configure(uint32 sampleRate,
								uint32 inputChannels = 1);
			void			Shutdown();

			uint32			SampleRate() const { return fSampleRate; }
			uint32			InputChannels() const { return fInputChannels; }

	// Called from the demodulator thread.
			void			Write(const float* samples, size_t count);
			void			WriteStereo(const float* samples, size_t frames);

			void			SetVolume(float gain);	// 0.0 - 1.0
			float			Volume() const { return fVolume; }
	// Ring capacity in seconds. Default 1 for the real-time path. The delayed
	// player writes whole multi-second chunks at once and needs a ring large
	// enough to hold one, or the excess is dropped as overruns. Call before
	// Configure().
			void			SetRingSeconds(int seconds);

			float			PeakLevel() const;
			uint64			Overruns() const;
			uint64			Underruns() const;
	// Frames BSoundPlayer has actually taken. This is the only direct
	// evidence that the output path is alive: with a silent source - which is
	// what the digital modes produce until there is a decoder - the level
	// meter reads zero whether the sound card is running or not.
			uint64			FramesPlayed() const;
			size_t			QueuedFrames() const;

private:
	static	void			_PlayProc(void* cookie, void* buffer, size_t size,
								const media_raw_audio_format& format);
			void			_Fill(float* out, size_t frames);
			void			_Write(const float* samples, size_t frames,
								uint32 channels);

			BSoundPlayer*	fPlayer;
			uint32			fSampleRate;
			uint32			fInputChannels;
			float			fVolume;
			int				fRingSeconds;

	mutable BLocker			fLock;
	std::vector<float>		fRing;
			size_t			fReadPos;
			size_t			fWritePos;
			size_t			fFill;
	// Playback stays muted until this much audio has accumulated, and the
	// cushion is rebuilt after every underrun. Without it, playback starts
	// on the first buffer that arrives and then lives permanently one
	// scheduling hiccup away from running dry.
			size_t			fPrimeFrames;
			bool			fPriming;

	// The dongle and the sound card do not run at the same rate, and the USB
	// path loses a little on top of that - measured at 0.9% short even with
	// 128 KB transfers, because every read has a fixed cost the samples keep
	// arriving during. No buffer size fixes a steady rate difference; it only
	// changes how long the ring takes to run dry, which is why enlarging it
	// moved the dropouts from every second to every two seconds instead of
	// removing them.
	//
	// So the reader runs at a fractional rate steered by how full the ring is.
	// Correcting a 1% shortfall means playing 1% slow, which is eight cents of
	// pitch and inaudible; a 250 ms silence every couple of seconds is not.
			double			fReadFrac;
			double			fRatio;

			float			fPeak;
			uint64			fOverruns;
			uint64			fUnderruns;
			uint64			fFramesPlayed;
};

#endif // RSDR_AUDIO_SINK_H
