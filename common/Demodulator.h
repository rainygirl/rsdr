/*
 * Analog demodulators: wideband FM, AM, airband AM, LSB, USB.
 *
 * Each mode gets its own sample-rate plan (see Demodulator.cpp for the
 * table and the reasoning). The two rules that shaped it:
 *
 * - The tuner is only asked for rates the RTL2832 can hit and that are an
 *   integer multiple of the audio rate, so no fractional resampler is needed
 *   anywhere in the chain.
 * - SSB is demodulated at 11025 Hz, not 44100. Separating two sidebands
 *   300 Hz apart needs a filter transition of a few hundred Hz, and the tap
 *   count for that scales with the sample rate - at 44100 it would be ~300
 *   taps, which this CPU cannot afford; at 11025 it is under 100.
 */
#ifndef RSDR_DEMODULATOR_H
#define RSDR_DEMODULATOR_H

#include <SupportDefs.h>

#include <vector>

#include "Dsp.h"

enum demod_mode {
	kModeWFM = 0,	// broadcast FM, 200 kHz channel
	kModeAM,
	// VHF airband AM, 118-137 MHz. Same envelope detector as kModeAM, but a
	// narrower channel to match 25 kHz spacing, frequencies shown in MHz
	// rather than kHz, and squelch on by default - an air frequency carries
	// nothing at all between transmissions, so an open squelch means full-gain
	// noise almost all of the time.
	kModeAir,
	kModeLSB,
	kModeUSB,
	kModeDMB,		// digital, handled by DabDecoder rather than this class

	kModeCount
};

bool ModeIsDigital(demod_mode mode);
const char* ModeName(demod_mode mode);

// The rate plan for one mode, so callers (Receiver, AudioSink) can set the
// tuner and the sound player up to match.
struct mode_plan {
	uint32	tunerSampleRate;
	uint32	audioSampleRate;
	// Nominal channel width, used to draw the passband on the spectrum and
	// to pick a sensible tuner IF bandwidth.
	uint32	channelBandwidth;
};

mode_plan PlanForMode(demod_mode mode);

class Demodulator {
public:
							Demodulator();

	// actualTunerRate is what rtlsdr_get_sample_rate() reported, which can
	// differ from the requested rate by a few Hz.
			void			Configure(demod_mode mode, uint32 actualTunerRate);

	// Offset of the wanted signal from the tuned frequency, in Hz. Lets SSB
	// be zero-beat and lets a station be nudged without a PLL retune.
			void			SetFineTune(float hz);
			void			SetSquelch(float dbfs) { fSquelchDb = dbfs; }
			void			SetDeemphasisTau(float seconds);
			void			SetFmStereo(bool enabled);
			bool			FmStereo() const { return fFmStereo; }

			void			Reset();

	// Converts one captured USB block and returns the number of audio
	// samples written. audioOut must have room for MaxAudioSamples(bytes).
			size_t			Process(const uint8* iq, size_t bytes,
								float* audioOut, size_t maxOut);
			size_t			MaxAudioSamples(size_t bytes) const;

	// Wideband power in the tuned channel, dB relative to full scale.
			float			SignalLevelDb() const { return fSignalDb; }
			bool			SquelchOpen() const { return fSquelchOpen; }

	// FM only: how far the carrier sits from where we are tuned, measured
	// from the DC term of the discriminator output. This is a direct readout
	// of the dongle's crystal error - on the unit developed against it read
	// -7.8 kHz at 92.5 MHz, or 84 ppm - so it doubles as the calibration aid
	// for the ppm setting. Zero for non-FM modes.
			float			CarrierOffsetHz() const { return fCarrierOffset; }

	// Complex baseband after channel filtering, kept for the spectrum
	// display. Valid until the next Process() call.
	const std::vector<dsp::Cf>& LastBaseband() const { return fSpectrumTap; }
			uint32			BasebandRate() const { return fBasebandRate; }

private:
			void			_BuildChain();
			size_t			_RunStages(size_t count);
			size_t			_ProcessFmStereo(float* audioOut, size_t maxOut,
								size_t count);

			demod_mode		fMode;
			mode_plan		fPlan;
			uint32			fTunerRate;
			float			fFineTuneHz;
			float			fSquelchDb;
			float			fDeemphasisTau;

			dsp::Nco		fNco;
	std::vector<dsp::ComplexDecimator> fStages;
			dsp::ComplexBandpass fSideband;
	std::vector<dsp::RealDecimator> fAudioStages;
	std::vector<dsp::RealDecimator> fStereoStages;
			dsp::Deemphasis		fDeemphasis;
			dsp::Deemphasis		fStereoDeemphasis;
			dsp::DcBlocker		fDcBlock;
			dsp::DcBlocker		fStereoDcBlock;
			dsp::Agc			fAgc;

	// Ping-pong scratch buffers for the decimation stages.
	std::vector<dsp::Cf>	fBufA;
	std::vector<dsp::Cf>	fBufB;
	std::vector<float>		fDemodBuf;
	std::vector<float>		fAudioBuf;
	std::vector<float>		fStereoBuf;
	std::vector<float>		fStereoWork;
	std::vector<float>		fStereoSum;
	std::vector<float>		fStereoDifference;
	std::vector<dsp::Cf>	fSpectrumTap;

			uint32			fBasebandRate;
			float			fSignalDb;
			bool			fSquelchOpen;
			float			fFmGain;
			float			fFmDeviation;
			float			fCarrierOffset;
			bool			fFmStereo;
			float			fStereoPilotPhase;
	// Fractional sample carried between blocks when generating silence for
	// the digital modes, whose tuner rate is not a multiple of their audio
	// rate.
			double			fDigitalAccumulator;
			dsp::Cf			fPrevSample;	// FM discriminator memory
};

#endif // RSDR_DEMODULATOR_H
