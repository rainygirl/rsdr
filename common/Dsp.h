/*
 * Signal-processing primitives for R SDR.
 *
 * Everything here is deliberately plain: contiguous float arrays, no
 * std::complex, no virtual calls in an inner loop. The target machine is a
 * single-core 1.33 GHz Atom, where the wideband-FM chain has to keep up with
 * a 1.06 MS/s complex input in real time - the FIR dot products below are
 * where essentially all of the CPU time goes.
 */
#ifndef RSDR_DSP_H
#define RSDR_DSP_H

#include <cstddef>
#include <vector>

namespace dsp {

// Interleaved-free complex sample. Kept as a POD struct of two floats so a
// vector<Cf> is bit-identical to an interleaved float array, which is what
// the FIR loops want.
struct Cf {
	float re;
	float im;

	Cf() : re(0.0f), im(0.0f) {}
	Cf(float r, float i) : re(r), im(i) {}
};

// atan2 accurate to about 1e-5 rad, roughly 10x cheaper than libm's on this
// CPU. The FM discriminator calls this once per IF sample (264600 times a
// second for wideband FM), which is enough for libm atan2f to show up in a
// profile.
float FastAtan2(float y, float x);

// Windowed-sinc (Hamming) lowpass. numTaps should be odd so the filter has
// an exact integer group delay.
void DesignLowpass(std::vector<float>& taps, int numTaps, float cutoffHz,
	float sampleRate);

// In-place radix-2 FFT, size must be a power of two. Only the spectrum
// display uses this - a couple of 1024-point transforms per second - so
// there is no reason to pull in fftw for it.
void Fft(Cf* data, int n);

// SSE2 decimation-in-frequency FFT whose output remains in bit-reversed
// order. Returns true when that fast path was used. Callers with an output-bin
// map can reverse the map once and avoid a permutation on every transform.
bool FftBitReversed(Cf* data, int n);

// Hann-windowed power spectrum in dB, DC moved to the centre bin so the
// display reads like a spectrum analyser (negative frequencies on the left).
// magsDb is resized to n.
void PowerSpectrumDb(const Cf* in, size_t inLen, int n,
	std::vector<float>& magsDb);

// Real-tap FIR that keeps only every decim'th output (polyphase in effect:
// the discarded outputs are never computed).
//
// State is carried between calls as a short history vector rather than a
// circular buffer, so every dot product runs over contiguous memory. See
// Process() for why the next window always starts at index 0.
class ComplexDecimator {
public:
					ComplexDecimator();

			void	Init(const std::vector<float>& taps, int decim);
			void	Reset();

	// Upper bound on the outputs a Process() call can produce, for sizing
	// the destination buffer.
			size_t	MaxOutput(size_t inLen) const;
			size_t	Process(const Cf* in, size_t inLen, Cf* out);

private:
	std::vector<float>	fTaps;
	std::vector<Cf>		fHist;
	std::vector<Cf>		fWork;
	int					fDecim;
};

// Same, for real-valued signals (post-demodulator audio).
class RealDecimator {
public:
					RealDecimator();

			void	Init(const std::vector<float>& taps, int decim);
			void	Reset();

			size_t	MaxOutput(size_t inLen) const;
			size_t	Process(const float* in, size_t inLen, float* out);

private:
	std::vector<float>	fTaps;
	std::vector<float>	fHist;
	std::vector<float>	fWork;
	int					fDecim;
};

// Complex-coefficient FIR: a real lowpass prototype shifted up to a centre
// frequency, so its passband is one-sided. That asymmetry is what separates
// the two SSB sidebands - a real filter cannot tell +1.5 kHz from -1.5 kHz.
class ComplexBandpass {
public:
					ComplexBandpass();

	// lowHz/highHz are signed offsets from the tuned frequency. For USB pass
	// roughly (+300, +2800); for LSB, (-2800, -300).
			void	Init(int numTaps, float lowHz, float highHz,
						float sampleRate);
			void	Reset();
			void	Process(const Cf* in, size_t inLen, Cf* out);
			bool	IsValid() const { return !fTaps.empty(); }

private:
	std::vector<Cf>		fTaps;
	std::vector<Cf>		fHist;
	std::vector<Cf>		fWork;
};

// Numerically controlled oscillator used for fine tuning (shifting the
// complex baseband left or right by a few kHz without retuning the tuner
// PLL, which would click and take milliseconds).
//
// Phase is a 32-bit accumulator indexing a sine table: exact frequency, no
// drift, and no trig calls in the loop.
class Nco {
public:
					Nco();

			void	SetFrequency(float hz, float sampleRate);
			void	Reset() { fPhase = 0; }
	// Set the accumulator to the phase this oscillator would have reached at
	// the given sample index. Needed when mixing a block that does not start
	// at sample zero: resetting the phase per block instead leaves a constant
	// offset between blocks, which for differentially demodulated OFDM is not
	// a cosmetic error - it rotates the whole constellation by 2*pi*df*Ts and
	// destroys the bit decisions while barely moving the MER.
			void	SetPhaseForSample(unsigned int index)
					{ fPhase = fStep * index; }
	// Multiplies in[] by exp(j*2*pi*f*t). in and out may alias.
			void	Mix(const Cf* in, size_t len, Cf* out);
			bool	IsZero() const { return fStep == 0; }

private:
	unsigned int	fPhase;
	unsigned int	fStep;
};

// Soft limiter for the very end of the audio path.
//
// Needed because an FM discriminator's output is not bounded by anything the
// receiver controls: full rated deviation maps to 1.0, and a station that
// over-modulates - or simply one whose carrier sits a few kHz off because the
// dongle's crystal is out - goes past it. Measured 1.52 on a local station,
// which hard-clips in the sound card and sounds like distortion. Below the
// threshold this is exactly transparent; above it, it compresses instead of
// truncating.
void SoftClip(float* buf, size_t len, float threshold);

// One-pole highpass, used to strip the carrier's DC term out of an AM
// envelope (and any residual DC offset the RTL2832's ADC contributes).
class DcBlocker {
public:
			DcBlocker() : fPrevIn(0.0f), fPrevOut(0.0f) {}
			void	Reset() { fPrevIn = 0.0f; fPrevOut = 0.0f; }
			void	Process(float* buf, size_t len);

private:
	float	fPrevIn;
	float	fPrevOut;
};

// FM de-emphasis: the transmitter pre-emphasises treble by a 50 us (Korea,
// Japan, Europe) or 75 us (Americas) time constant, and this undoes it.
// Without it, broadcast FM sounds harsh and hissy rather than wrong, which
// makes it easy to forget.
class Deemphasis {
public:
			Deemphasis() : fAlpha(1.0f), fState(0.0f) {}
			void	Init(float tauSeconds, float sampleRate);
			void	Reset() { fState = 0.0f; }
			void	Process(float* buf, size_t len);

private:
	float	fAlpha;
	float	fState;
};

// Peak-tracking AGC for AM and SSB, whose output level otherwise follows
// the received field strength over a 60 dB range. Fast attack so a sudden
// strong signal does not blast, slow decay so speech pauses do not pump.
class Agc {
public:
					Agc();
			void	Init(float sampleRate, float target);
			void	Reset();
			void	Process(float* buf, size_t len);
			float	Gain() const { return fGain; }

private:
	float	fTarget;
	float	fAttack;
	float	fDecay;
	float	fEnvelope;
	float	fGain;
};

} // namespace dsp

#endif // RSDR_DSP_H
