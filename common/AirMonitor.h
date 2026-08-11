/*
 * Watches a whole list of airband channels at once and plays whichever one is
 * transmitting.
 *
 * An air frequency is silent except while somebody is talking, so listening to
 * one channel means hearing nothing almost all of the time. Stepping a scanner
 * from channel to channel is the usual answer and it is a poor one: a retune
 * only takes effect between two USB reads, so the scanner spends its time
 * looking at the wrong frequency and clips the start of every transmission.
 *
 * The trick used here is that detecting a signal and demodulating it are very
 * different costs. One FFT of the wideband block gives the power in *every*
 * channel inside the tuner window simultaneously, for the price of one FFT.
 * Only the channel that turns out to be active is actually demodulated. So a
 * window's worth of channels is monitored continuously, with no switching
 * delay and no missed openings, and the CPU cost barely depends on how many
 * channels are in the list.
 *
 * A city list still spans more than one tuner window - Seoul's runs from 118
 * to 133 MHz and the tuner sees 2.048 MHz at a time - so windows are visited
 * in turn. The monitor stays on a window while anything in it is active and
 * moves on when the window has been quiet for a moment, which means the
 * scanning only happens across groups, not across channels.
 */
#ifndef RSDR_AIR_MONITOR_H
#define RSDR_AIR_MONITOR_H

#include <SupportDefs.h>

#include <string>
#include <vector>

#include "Dsp.h"

class AirMonitor {
public:
	// 2.048 MS/s: the widest rate this dongle delivers reliably, and it puts
	// most of an airport's tower/ground/approach channels in one window.
	static const uint32 kTunerRate = 2048000;
	static const uint32 kAudioRate = 16000;

	struct channel {
		std::string	label;
		uint64		hz;
		float		levelDb;
		float		floorDb;
		bool		open;

		channel() : hz(0), levelDb(-140.0f), floorDb(-140.0f), open(false) {}
	};

	// A set of channels that fit inside one tuner window.
	struct window {
		uint64				centreHz;
		std::vector<int>	channels;	// indices into the channel list

		window() : centreHz(0) {}
	};

						AirMonitor();

	// Groups the channels into tuner windows. Channels further apart than the
	// usable window width end up in different windows.
			void		SetChannels(const std::vector<channel>& channels);
	// Margin in dB a channel must stand above the measured noise floor before
	// it counts as active. The default is deliberately low: on a quiet band
	// this hardware reads +5 to +9 dB on noise alone, so a high threshold is
	// safe but silent, and hearing occasional noise is better than missing
	// traffic. Raise it with the squelch slider if the band is busy.
			void		SetThresholdDb(float db) { fThresholdDb = db; }
			float		ThresholdDb() const { return fThresholdDb; }
			void		Reset();

			int			WindowCount() const { return (int)fWindows.size(); }
			int			CurrentWindow() const { return fWindow; }
	// Centre the tuner should be on right now.
			uint64		TuneHz() const;
	// True when the caller should retune, which it clears by calling TuneHz().
			bool		NeedsRetune() const { return fRetune; }
			void		RetuneDone() { fRetune = false; }

	// One captured block at kTunerRate. Appends audio at kAudioRate and
	// returns how many frames were appended; sets active to the channel index
	// being played, or -1 for silence.
			size_t		Process(const uint8* iq, size_t bytes,
							std::vector<float>& out, int& active);

	const std::vector<channel>& Channels() const { return fChannels; }
			int			ActiveChannel() const { return fActive; }

private:
			void		_MeasureLevels(const dsp::Cf* x, size_t count);
			void		_ConfigureFor(int channelIndex);

	std::vector<channel>	fChannels;
	std::vector<window>		fWindows;
			int				fWindow;
			bool			fRetune;
			int				fActive;
			float			fThresholdDb;
			bigtime_t		fActiveAt;
			bigtime_t		fWindowAt;

	// Detection: one FFT per block over the whole window.
	std::vector<dsp::Cf>	fFftBuf;
	std::vector<float>		fPower;

	// Demodulation: built for whichever channel is currently active.
			dsp::Nco		fNco;
	std::vector<dsp::ComplexDecimator>	fStages;
	std::vector<dsp::Cf>	fBufA;
	std::vector<dsp::Cf>	fBufB;
	std::vector<float>		fDemod;
			dsp::DcBlocker	fDcBlock;
			dsp::Agc		fAgc;
			int				fConfiguredFor;
			uint64			fSampleIndex;
};

#endif // RSDR_AIR_MONITOR_H
