#include "Demodulator.h"

#include <cmath>
#include <cstring>

namespace {

const float kTwoPi = 6.28318530717958647692f;

// Tuner rates. Both are inside a range the RTL2832 can actually produce -
// its divider cannot synthesise anything between 300 kHz and 900 kHz, so the
// obvious "half a megahertz" choices are unavailable - and both are exact
// integer multiples of 44100, which is what keeps a fractional resampler out
// of the signal path entirely.
//
//   264600 = 44100 * 6
//  1058400 = 44100 * 24
const uint32 kNarrowTunerRate = 264600;
const uint32 kWideTunerRate = 1058400;

// DAB Mode I. Not a free choice: see the kModeDMB case below.
const uint32 kDabTunerRate = 2048000;

struct stage_spec {
	int		decim;
	int		taps;
	float	cutoffHz;
};

// A decimating FIR only needs its stopband to begin at the *new* Nyquist
// frequency: everything above that is what folds, and the filter runs before
// the decimation, so anything it attenuates cannot alias. That is the whole
// sizing rule for the tap counts below. A Hamming-windowed sinc reaches its
// -43 dB stopband about 3.3 * sampleRate / numTaps above the cutoff.
struct chain_spec {
	uint32		tunerRate;
	uint32		audioRate;
	uint32		channelBandwidth;
	stage_spec	stages[2];
	int			stageCount;
	// Post-demodulator real filtering. Two stages, because one is not enough
	// for wideband FM - see below. decim 1 means filter only, taps 0 means
	// neither.
	stage_spec	audioStages[2];
	int			audioStageCount;
	float		fmDeviation;	// 0 for non-FM modes
};

chain_spec
ChainForMode(demod_mode mode)
{
	chain_spec c;
	memset(&c, 0, sizeof(c));

	switch (mode) {
		case kModeWFM:
			// Sampled at 264600 and discriminated with no complex decimation
			// at all: +-132 kHz is enough for a 75 kHz-deviation broadcast
			// signal, and the discriminator is a nonlinear detector where
			// the capture effect suppresses a weaker neighbour anyway.
			// (rtl_fm's own wideband mode uses 170 kHz, less than this.)
			//
			// The alternative - 1058400 with a 51-tap complex decimator down
			// to 264600 - gives better adjacent-channel rejection and was
			// what this did first. It does not fit on this machine. It costs
			// four times the USB bandwidth and four times the filter work,
			// and on a single-core Atom the two together starved libusb's
			// transfer thread badly enough that 3% of the samples never
			// arrived, which is audible as a tick roughly once a second.
			// Selectivity that only exists in theory is worth less than
			// audio that does not stutter.
			c.tunerRate = kNarrowTunerRate;
			c.audioRate = 44100;
			c.channelBandwidth = 200000;
			c.stageCount = 0;
			// The audio decimation from 264600 to 44100 is split in two, and
			// this is not an optimisation - a single 151-tap filter here made
			// the audio audibly grainy.
			//
			// A stereo FM multiplex is not a 15 kHz audio signal. Measured on
			// a local station: the 19 kHz pilot sits 12 dB *above* the
			// average level of the 300 Hz - 5 kHz audio band, and the
			// 23-53 kHz L-R subcarrier is at roughly the same level as the
			// audio, as is RDS at 57 kHz. Decimating by 6 folds all of that
			// straight into the passband - 38 kHz lands on 6.1 kHz, 57 kHz on
			// 12.9 kHz - so the filter's stopband depth, not its cutoff, is
			// what decides how clean the result is. A single Hamming-windowed
			// stage gives 43 dB, against interference starting at the same
			// level as the wanted audio.
			//
			// Two stages multiply their attenuations in the region that
			// matters: /3 first, which puts 38 kHz and up deep in its
			// stopband, then /2 with a much sharper filter (its transition
			// only has to span 14.5 to 22 kHz at a third of the rate), which
			// also kills the pilot. The pair costs slightly less than the one
			// 151-tap filter did.
			c.audioStages[0].decim = 3;
			c.audioStages[0].taps = 31;
			c.audioStages[0].cutoffHz = 15000.0f;
			c.audioStages[1].decim = 2;
			c.audioStages[1].taps = 73;
			c.audioStages[1].cutoffHz = 14500.0f;
			c.audioStageCount = 2;
			c.fmDeviation = 75000.0f;
			break;

		case kModeAM:
			c.tunerRate = kNarrowTunerRate;
			c.audioRate = 22050;
			c.channelBandwidth = 12000;
			c.stages[0].decim = 6;
			c.stages[0].taps = 121;
			c.stages[0].cutoffHz = 6000.0f;
			c.stages[1].decim = 2;
			c.stages[1].taps = 61;
			c.stages[1].cutoffHz = 6000.0f;
			c.stageCount = 2;
			// Envelope detection needs no post-filter: the complex stages
			// already limited the channel.
			c.audioStageCount = 0;
			break;

		case kModeAir:
			// Airband AM voice occupies about 6 kHz inside a 25 kHz channel,
			// so the passband is tighter than broadcast AM. That is worth real
			// sensitivity here: the signals are line-of-sight from an aircraft
			// and often weak, and the adjacent channel is only 25 kHz away.
			c.tunerRate = kNarrowTunerRate;
			c.audioRate = 22050;
			c.channelBandwidth = 8000;
			c.stages[0].decim = 6;
			c.stages[0].taps = 121;
			c.stages[0].cutoffHz = 4000.0f;
			c.stages[1].decim = 2;
			c.stages[1].taps = 61;
			c.stages[1].cutoffHz = 4000.0f;
			c.stageCount = 2;
			c.audioStageCount = 0;
			break;

		case kModeLSB:
		case kModeUSB:
			// Down to 11025 Hz before the sideband filter - see the header.
			c.tunerRate = kNarrowTunerRate;
			c.audioRate = 11025;
			c.channelBandwidth = 3000;
			c.stages[0].decim = 6;
			c.stages[0].taps = 121;
			c.stages[0].cutoffHz = 5000.0f;
			c.stages[1].decim = 4;
			c.stages[1].taps = 97;
			c.stages[1].cutoffHz = 3200.0f;
			c.stageCount = 2;
			// The one-sided FIR is the channel filter.
			c.audioStageCount = 0;
			break;

		case kModeDMB:
			// DAB Mode I is defined at 2.048 MS/s and nothing else will do:
			// the symbol, guard and frame lengths are all whole numbers of
			// samples only at that rate. No demodulation happens in this
			// class for it - DabProbe does the synchronisation - but the rate
			// and the 1.536 MHz block width belong in the plan so the tuner
			// and the spectrum display are set up correctly.
			c.tunerRate = kDabTunerRate;
			c.audioRate = 48000;		// what DAB audio services use
			c.channelBandwidth = 1536000;
			c.stageCount = 0;
			c.audioStageCount = 0;
			break;

		default:
			break;
	}
	return c;
}

} // namespace

bool
ModeIsDigital(demod_mode mode)
{
	return mode == kModeDMB;
}

const char*
ModeName(demod_mode mode)
{
	switch (mode) {
		case kModeWFM:	return "FM";
		case kModeAM:	return "AM";
		case kModeAir:	return "Air";
		case kModeLSB:	return "LSB";
		case kModeUSB:	return "USB";
		case kModeDMB:	return "T-DMB";
		default:		return "?";
	}
}

mode_plan
PlanForMode(demod_mode mode)
{
	chain_spec c = ChainForMode(mode);
	mode_plan plan;
	plan.tunerSampleRate = c.tunerRate;
	plan.audioSampleRate = c.audioRate;
	plan.channelBandwidth = c.channelBandwidth;
	return plan;
}

Demodulator::Demodulator()
	:
	fMode(kModeWFM),
	fTunerRate(kWideTunerRate),
	fFineTuneHz(0.0f),
	fSquelchDb(-200.0f),
	fDeemphasisTau(50e-6f),
	fBasebandRate(kWideTunerRate),
	fSignalDb(-120.0f),
	fSquelchOpen(true),
	fFmGain(1.0f),
	fFmDeviation(0.0f),
	fCarrierOffset(0.0f),
	fFmStereo(false),
	fStereoPilotPhase(0.0f),
	fDigitalAccumulator(0.0)
{
	fPlan = PlanForMode(kModeWFM);
	Configure(kModeWFM, kWideTunerRate);
}

void
Demodulator::Configure(demod_mode mode, uint32 actualTunerRate)
{
	fMode = mode;
	fPlan = PlanForMode(mode);
	fTunerRate = actualTunerRate > 0 ? actualTunerRate : fPlan.tunerSampleRate;
	_BuildChain();
	Reset();
}

void
Demodulator::SetFineTune(float hz)
{
	fFineTuneHz = hz;
	// Negated: to hear a signal sitting at +2 kHz, the baseband has to be
	// shifted down by 2 kHz to bring it to zero.
	fNco.SetFrequency(-hz, (float)fTunerRate);
}

void
Demodulator::SetDeemphasisTau(float seconds)
{
	fDeemphasisTau = seconds;
	fDeemphasis.Init(seconds, (float)fPlan.audioSampleRate);
	fStereoDeemphasis.Init(seconds, (float)fPlan.audioSampleRate);
}

void
Demodulator::SetFmStereo(bool enabled)
{
	if (fFmStereo == enabled)
		return;
	fFmStereo = enabled;
	Reset();
}

void
Demodulator::_BuildChain()
{
	chain_spec c = ChainForMode(fMode);

	fStages.clear();
	fStages.resize(c.stageCount);

	float rate = (float)fTunerRate;
	for (int i = 0; i < c.stageCount; i++) {
		std::vector<float> taps;
		dsp::DesignLowpass(taps, c.stages[i].taps, c.stages[i].cutoffHz, rate);
		fStages[i].Init(taps, c.stages[i].decim);
		rate /= (float)c.stages[i].decim;
	}

	// Rate the demodulator itself runs at.
	float demodRate = rate;

	fAudioStages.clear();
	fAudioStages.resize(c.audioStageCount);
	fStereoStages.clear();
	fStereoStages.resize(c.audioStageCount);
	float audioRate = demodRate;
	for (int i = 0; i < c.audioStageCount; i++) {
		std::vector<float> taps;
		if (c.audioStages[i].taps > 0) {
			dsp::DesignLowpass(taps, c.audioStages[i].taps,
				c.audioStages[i].cutoffHz, audioRate);
		}
		fAudioStages[i].Init(taps, c.audioStages[i].decim);
		fStereoStages[i].Init(taps, c.audioStages[i].decim);
		audioRate /= (float)c.audioStages[i].decim;
	}

	if (fMode == kModeUSB) {
		// 300 Hz to 2.9 kHz above the carrier. Starting at 300 rather than 0
		// keeps the AGC from chasing the low-frequency rumble that a carrier
		// a few hundred Hz off zero-beat produces.
		fSideband.Init(97, 300.0f, 2900.0f, demodRate);
	} else if (fMode == kModeLSB) {
		fSideband.Init(97, -2900.0f, -300.0f, demodRate);
	} else {
		fSideband = dsp::ComplexBandpass();
	}

	fFmDeviation = c.fmDeviation;
	if (c.fmDeviation > 0.0f)
		fFmGain = demodRate / (kTwoPi * c.fmDeviation);
	else
		fFmGain = 1.0f;

	fDeemphasis.Init(fDeemphasisTau, (float)c.audioRate);
	fStereoDeemphasis.Init(fDeemphasisTau, (float)c.audioRate);
	fAgc.Init((float)c.audioRate, 0.25f);
	fBasebandRate = fTunerRate;

	fNco.SetFrequency(-fFineTuneHz, (float)fTunerRate);
}

void
Demodulator::Reset()
{
	for (size_t i = 0; i < fStages.size(); i++)
		fStages[i].Reset();
	fSideband.Reset();
	for (size_t i = 0; i < fAudioStages.size(); i++)
		fAudioStages[i].Reset();
	for (size_t i = 0; i < fStereoStages.size(); i++)
		fStereoStages[i].Reset();
	fDeemphasis.Reset();
	fStereoDeemphasis.Reset();
	fDcBlock.Reset();
	fStereoDcBlock.Reset();
	fAgc.Reset();
	fNco.Reset();
	fPrevSample = dsp::Cf(0.0f, 0.0f);
	fSignalDb = -120.0f;
	fSquelchOpen = true;
	fCarrierOffset = 0.0f;
	fStereoPilotPhase = 0.0f;
	fDigitalAccumulator = 0.0;
}

size_t
Demodulator::MaxAudioSamples(size_t bytes) const
{
	// Mirrors what the decimators actually do rather than just dividing by
	// the total decimation. Each stage can emit one extra sample beyond the
	// nominal ratio (it carries a partial window over from the previous
	// call), and with the audio decimation split in two those roundings
	// compound - the naive bound came out 34 samples short for wideband FM,
	// which is a buffer overrun, not a rounding error.
	size_t n = bytes / 2;
	chain_spec c = ChainForMode(fMode);
	for (int i = 0; i < c.stageCount; i++) {
		size_t taps = c.stages[i].taps > 0 ? (size_t)c.stages[i].taps : 1;
		size_t decim = c.stages[i].decim > 0 ? (size_t)c.stages[i].decim : 1;
		n = (n + taps) / decim + 1;
	}
	for (int i = 0; i < c.audioStageCount; i++) {
		size_t taps = c.audioStages[i].taps > 0
			? (size_t)c.audioStages[i].taps : 1;
		size_t decim = c.audioStages[i].decim > 0
			? (size_t)c.audioStages[i].decim : 1;
		n = (n + taps) / decim + 1;
	}
	return (n + 8) * (fMode == kModeWFM && fFmStereo ? 2 : 1);
}

size_t
Demodulator::_ProcessFmStereo(float* audioOut, size_t maxOut, size_t count)
{
	// Recover the 38 kHz stereo subcarrier coherently from the 19 kHz pilot.
	// One correlation over a USB block is stable on weak signals and avoids a
	// sample-by-sample PLL adding its own warble to the stereo image.
	const float step = kTwoPi * 19000.0f / (float)fTunerRate;
	const float startPhase = fStereoPilotPhase;
	float phase = startPhase;
	double iPilot = 0.0;
	double qPilot = 0.0;
	double power = 0.0;
	for (size_t i = 0; i < count; i++) {
		float v = fDemodBuf[i];
		iPilot += (double)v * cosf(phase);
		qPilot += (double)v * sinf(phase);
		power += (double)v * v;
		phase += step;
		if (phase >= kTwoPi)
			phase -= kTwoPi;
	}
	fStereoPilotPhase = phase;

	float pilotAmplitude = 2.0f * (float)sqrt(iPilot * iPilot
		+ qPilot * qPilot) / (float)count;
	float rms = sqrtf((float)(power / (double)count));
	bool pilotLocked = pilotAmplitude > 0.012f
		&& pilotAmplitude > rms * 0.025f;
	// qPilot has the opposite sign for a cosine-reference phase estimate.
	float offset = atan2f((float)-qPilot, (float)iPilot);

	fStereoDifference.resize(count);
	phase = startPhase;
	for (size_t i = 0; i < count; i++) {
		fStereoDifference[i] = pilotLocked
			? 2.0f * fDemodBuf[i] * cosf(2.0f * (phase + offset)) : 0.0f;
		phase += step;
		if (phase >= kTwoPi)
			phase -= kTwoPi;
	}

	// Identical filters keep L+R and L-R sample-aligned while rejecting the
	// pilot, RDS and mixer products and decimating to 44.1 kHz.
	const float* sumSrc = &fDemodBuf[0];
	const float* diffSrc = &fStereoDifference[0];
	size_t sumCount = count;
	size_t diffCount = count;
	for (size_t s = 0; s < fAudioStages.size(); s++) {
		fStereoSum.resize(fAudioStages[s].MaxOutput(sumCount));
		fStereoWork.resize(fStereoStages[s].MaxOutput(diffCount));
		sumCount = fAudioStages[s].Process(sumSrc, sumCount, &fStereoSum[0]);
		diffCount = fStereoStages[s].Process(diffSrc, diffCount,
			&fStereoWork[0]);
		fAudioBuf.swap(fStereoSum);
		fStereoBuf.swap(fStereoWork);
		sumSrc = &fAudioBuf[0];
		diffSrc = &fStereoBuf[0];
	}

	size_t frames = sumCount < diffCount ? sumCount : diffCount;
	if (frames > maxOut / 2)
		frames = maxOut / 2;
	if (frames == 0)
		return 0;

	if (fFmDeviation > 0.0f) {
		double sum = 0.0;
		for (size_t i = 0; i < frames; i++)
			sum += sumSrc[i];
		float mean = (float)(sum / (double)frames);
		fCarrierOffset += 0.05f
			* (mean * fFmDeviation - fCarrierOffset);
	}

	fStereoSum.assign(sumSrc, sumSrc + frames);
	fStereoDifference.assign(diffSrc, diffSrc + frames);
	fDcBlock.Process(&fStereoSum[0], frames);
	fStereoDcBlock.Process(&fStereoDifference[0], frames);
	// Matrixing without an extra 1/2 preserves the existing mono loudness:
	// the transmitted multiplex already contains half-sum and half-difference.
	for (size_t i = 0; i < frames; i++) {
		float sum = fStereoSum[i];
		float difference = fStereoDifference[i];
		fStereoSum[i] = sum + difference;
		fStereoDifference[i] = sum - difference;
	}
	fDeemphasis.Process(&fStereoSum[0], frames);
	fStereoDeemphasis.Process(&fStereoDifference[0], frames);
	for (size_t i = 0; i < frames; i++) {
		audioOut[2 * i] = fStereoSum[i];
		audioOut[2 * i + 1] = fStereoDifference[i];
	}
	if (!fSquelchOpen)
		memset(audioOut, 0, frames * 2 * sizeof(float));
	dsp::SoftClip(audioOut, frames * 2, 0.70f);
	return frames * 2;
}

size_t
Demodulator::Process(const uint8* iq, size_t bytes, float* audioOut,
	size_t maxOut)
{
	const size_t inSamples = bytes / 2;
	if (inSamples == 0)
		return 0;
	// audioOut may be NULL for the digital modes, which want only the level
	// and the spectrum tap.

	fBufA.resize(inSamples);
	// The RTL2832 delivers offset-binary unsigned bytes; 127.5 is the true
	// zero, and dividing by it puts full scale at magnitude 1.0 so the level
	// readout is honest dBFS.
	const float scale = 1.0f / 127.5f;
	double power = 0.0;
	for (size_t i = 0; i < inSamples; i++) {
		float re = ((float)iq[2 * i] - 127.5f) * scale;
		float im = ((float)iq[2 * i + 1] - 127.5f) * scale;
		fBufA[i].re = re;
		fBufA[i].im = im;
		power += (double)re * re + (double)im * im;
	}

	fSignalDb = 10.0f * log10f((float)(power / (double)inSamples) + 1e-12f);

	// Spectrum tap: raw tuner-rate baseband, which is the widest view
	// available and therefore the useful one for finding a signal.
	{
		const size_t want = 4096;
		size_t n = inSamples < want ? inSamples : want;
		fSpectrumTap.assign(fBufA.begin(), fBufA.begin() + n);
	}

	// The digital modes stop here: the level and the spectrum tap above are
	// all this class produces for them. Receiver feeds the sound card for
	// these modes, from the wall clock rather than from the sample count -
	// see Receiver::_DemodLoop for why that distinction matters.
	if (ModeIsDigital(fMode))
		return 0;

	if (!fNco.IsZero())
		fNco.Mix(&fBufA[0], inSamples, &fBufA[0]);

	// Decimation stages, ping-ponging between the two buffers.
	dsp::Cf* src = &fBufA[0];
	size_t count = inSamples;
	for (size_t s = 0; s < fStages.size(); s++) {
		std::vector<dsp::Cf>& dstVec = (s % 2 == 0) ? fBufB : fBufA;
		dstVec.resize(fStages[s].MaxOutput(count));
		count = fStages[s].Process(src, count, &dstVec[0]);
		src = &dstVec[0];
	}

	if (count == 0)
		return 0;

	fSquelchOpen = fSignalDb >= fSquelchDb;

	fDemodBuf.resize(count);

	switch (fMode) {
		case kModeWFM:
		{
			// Phase difference between consecutive samples is the
			// instantaneous frequency. Computed as arg(z[n] * conj(z[n-1]))
			// so no unwrapping is ever needed.
			dsp::Cf prev = fPrevSample;
			for (size_t i = 0; i < count; i++) {
				dsp::Cf z = src[i];
				float cross = z.re * prev.re + z.im * prev.im;
				float quad = z.im * prev.re - z.re * prev.im;
				fDemodBuf[i] = dsp::FastAtan2(quad, cross) * fFmGain;
				prev = z;
			}
			fPrevSample = prev;
			break;
		}

		case kModeAM:
		case kModeAir:
			for (size_t i = 0; i < count; i++) {
				fDemodBuf[i] = sqrtf(src[i].re * src[i].re
					+ src[i].im * src[i].im);
			}
			break;

		case kModeLSB:
		case kModeUSB:
		{
			// One-sided filter, then take the real part: the wanted sideband
			// already sits at audio frequencies, and its mirror at negative
			// frequency is the same real signal.
			fBufB.resize(count);
			fSideband.Process(src, count, &fBufB[0]);
			for (size_t i = 0; i < count; i++)
				fDemodBuf[i] = 2.0f * fBufB[i].re;
			break;
		}

		default:
			for (size_t i = 0; i < count; i++)
				fDemodBuf[i] = 0.0f;
			break;
	}

	if (fMode == kModeWFM && fFmStereo)
		return _ProcessFmStereo(audioOut, maxOut, count);

	// Post-demodulator real filtering / decimation, ping-ponging between the
	// caller's buffer and a scratch one so the last stage always lands in
	// audioOut.
	size_t produced = count;
	if (fAudioStages.empty()) {
		if (produced > maxOut)
			produced = maxOut;
		memcpy(audioOut, &fDemodBuf[0], produced * sizeof(float));
	} else {
		const float* src = &fDemodBuf[0];
		for (size_t s = 0; s < fAudioStages.size(); s++) {
			bool last = (s + 1 == fAudioStages.size());
			float* dst;
			if (last) {
				dst = audioOut;
			} else {
				fAudioBuf.resize(fAudioStages[s].MaxOutput(produced));
				dst = &fAudioBuf[0];
			}
			produced = fAudioStages[s].Process(src, produced, dst);
			src = dst;
		}
	}
	if (produced > maxOut)
		produced = maxOut;

	switch (fMode) {
		case kModeWFM:
		{
			// The discriminator's DC term is the difference between where the
			// tuner is and where the carrier actually is. On a cheap dongle
			// that is dominated by crystal error, not by anything the station
			// is doing: this one reads 7.8 kHz at 92.5 MHz. Left in, it is a
			// large constant offset that eats headroom and makes the level
			// meter meaningless - and after de-emphasis it becomes a thump on
			// every retune.
			if (produced > 0 && fFmDeviation > 0.0f) {
				double sum = 0.0;
				for (size_t i = 0; i < produced; i++)
					sum += audioOut[i];
				float mean = (float)(sum / (double)produced);
				// Slow average: a single block of loud bass would otherwise
				// look like a tuning error.
				fCarrierOffset += 0.05f
					* (mean * fFmDeviation - fCarrierOffset);
			}
			fDcBlock.Process(audioOut, produced);

			if (fMode == kModeWFM)
				fDeemphasis.Process(audioOut, produced);
			if (!fSquelchOpen)
				memset(audioOut, 0, produced * sizeof(float));
			break;
		}

		case kModeAM:
		case kModeAir:
			fDcBlock.Process(audioOut, produced);
			fAgc.Process(audioOut, produced);
			if (!fSquelchOpen)
				memset(audioOut, 0, produced * sizeof(float));
			break;

		case kModeLSB:
		case kModeUSB:
			fAgc.Process(audioOut, produced);
			if (!fSquelchOpen)
				memset(audioOut, 0, produced * sizeof(float));
			break;

		default:
			break;
	}

	// Last thing in the chain, for every mode: nothing upstream guarantees
	// the result fits in +-1.0.
	dsp::SoftClip(audioOut, produced, 0.70f);

	return produced;
}
