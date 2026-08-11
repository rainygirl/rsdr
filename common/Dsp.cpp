#include "Dsp.h"

#include <cmath>
#include <cstring>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

#if defined(__APPLE__) && defined(RSDR_USE_ACCELERATE)
#include <Accelerate/Accelerate.h>
#endif

namespace dsp {

namespace {

const float kPi = 3.14159265358979323846f;

#if defined(__SSE2__)
struct FftPlanSse2 {
	int n;
	std::vector<unsigned short> bitrev;
	std::vector<Cf> twiddle;
	std::vector<int> offset;
	FftPlanSse2() : n(0) {}
	void Prepare(int size)
	{
		if (n == size)
			return;
		n = size;
		bitrev.resize((size_t)n);
		for (int i = 0; i < n; i++) {
			int x = i, r = 0;
			for (int b = 1; b < n; b <<= 1) {
				r = (r << 1) | (x & 1);
				x >>= 1;
			}
			bitrev[(size_t)i] = (unsigned short)r;
		}
		offset.clear();
		twiddle.clear();
		for (int len = 2; len <= n; len <<= 1) {
			offset.push_back((int)twiddle.size());
			int half = len / 2;
			for (int k = 0; k < half; k++) {
				float a = -2.0f * kPi * (float)k / (float)len;
				twiddle.push_back(Cf(cosf(a), sinf(a)));
			}
		}
	}
};

inline __m128
ComplexMulPair(__m128 value, const Cf* twiddle)
{
	__m128 re = _mm_shuffle_ps(value, value, _MM_SHUFFLE(2, 0, 2, 0));
	__m128 im = _mm_shuffle_ps(value, value, _MM_SHUFFLE(3, 1, 3, 1));
	__m128 wr = _mm_setr_ps(twiddle[0].re, twiddle[1].re,
		twiddle[0].re, twiddle[1].re);
	__m128 wi = _mm_setr_ps(twiddle[0].im, twiddle[1].im,
		twiddle[0].im, twiddle[1].im);
	__m128 outRe = _mm_sub_ps(_mm_mul_ps(re, wr), _mm_mul_ps(im, wi));
	__m128 outIm = _mm_add_ps(_mm_mul_ps(re, wi), _mm_mul_ps(im, wr));
	return _mm_unpacklo_ps(outRe, outIm);
}

// SSE2 radix-2 path. Two independent butterflies in adjacent groups share
// the same twiddle factor, so their real/imaginary lanes can be processed
// together. The scalar fallback remains available for non-SSE2 hosts.
bool
FftSse2(Cf* data, int n)
{
	if (n < 2)
		return true;
	if ((n & (n - 1)) != 0 || n > 65535)
		return false;
	static thread_local FftPlanSse2 plan;
	plan.Prepare(n);
	for (int i = 0; i < n; i++) {
		int j = plan.bitrev[(size_t)i];
		if (i < j) {
			Cf t = data[i]; data[i] = data[j]; data[j] = t;
		}
	}
	// Radix-4 first pass: the len=2 and len=4 stages have only trivial twiddles
	// (1 and -i), so fuse them into a single memory pass over the bit-reversed
	// data. That removes one of the eleven passes for n=2048 and all of their
	// complex multiplies. The remaining stages run radix-2 from len=8.
	for (int base = 0; base + 4 <= n; base += 4) {
		Cf a = data[base], b = data[base + 1];
		Cf c = data[base + 2], d = data[base + 3];
		Cf ap(a.re + b.re, a.im + b.im);
		Cf bp(a.re - b.re, a.im - b.im);
		Cf cp(c.re + d.re, c.im + d.im);
		Cf dp(c.re - d.re, c.im - d.im);
		Cf t(dp.im, -dp.re); // -i * dp
		data[base] = Cf(ap.re + cp.re, ap.im + cp.im);
		data[base + 1] = Cf(bp.re + t.re, bp.im + t.im);
		data[base + 2] = Cf(ap.re - cp.re, ap.im - cp.im);
		data[base + 3] = Cf(bp.re - t.re, bp.im - t.im);
	}
	// The remaining stages stay radix-2: a scalar radix-4 fusion of these was
	// tried and reverted - halving the memory passes did not pay for losing the
	// SSE butterfly (FFT rose from 21729 to 30918 ticks on the Atom). A radix-4
	// win here would need an SSE radix-4 butterfly, not a scalar one.
	for (int len = (n >= 4 ? 8 : 2); len <= n; len <<= 1) {
		int stage = 0;
		for (int x = len; x > 2; x >>= 1) stage++;
		const Cf* tw = &plan.twiddle[(size_t)plan.offset[(size_t)stage]];
		int half = len / 2;
		for (int base = 0; base < n; base += len) {
			int k = 0;
			for (; k + 1 < half; k += 2) {
				// Pack two complex values as [re0,im0,re1,im1].  The
				// even/odd shuffles duplicate real and imaginary lanes so
				// both complex products are evaluated in four SSE lanes.
				int b0 = base + k, b1 = b0 + half;
				__m128 v = _mm_loadu_ps(reinterpret_cast<const float*>(data + b1));
				__m128 u = _mm_loadu_ps(reinterpret_cast<const float*>(data + b0));
				__m128 ve = _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 0, 2, 0));
				__m128 vo = _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 1, 3, 1));
				__m128 we = _mm_setr_ps(tw[k].re, tw[k + 1].re,
					tw[k].re, tw[k + 1].re);
				__m128 wo = _mm_setr_ps(tw[k].im, tw[k + 1].im,
					tw[k].im, tw[k + 1].im);
				__m128 vr = _mm_sub_ps(_mm_mul_ps(ve, we), _mm_mul_ps(vo, wo));
				__m128 vi = _mm_add_ps(_mm_mul_ps(ve, wo), _mm_mul_ps(vo, we));
				__m128 p = _mm_unpacklo_ps(vr, vi);
				_mm_storeu_ps(reinterpret_cast<float*>(data + b0), _mm_add_ps(u, p));
				_mm_storeu_ps(reinterpret_cast<float*>(data + b1), _mm_sub_ps(u, p));
			}
			for (; k < half; k++) {
				int b0 = base + k, b1 = b0 + half;
				Cf u = data[b0], v = data[b1];
				float vr = v.re * tw[k].re - v.im * tw[k].im;
				float vi = v.re * tw[k].im + v.im * tw[k].re;
				data[b0].re = u.re + vr; data[b0].im = u.im + vi;
				data[b1].re = u.re - vr; data[b1].im = u.im - vi;
			}
		}
	}
	return true;
}

// Decimation in frequency produces the same transform in bit-reversed order.
// DabSync consumes mapped carriers, so it can reverse that map once and save
// the permutation on every OFDM symbol.
bool
FftSse2BitReversed(Cf* data, int n)
{
	if (n < 2)
		return true;
	if ((n & (n - 1)) != 0 || n > 65535)
		return false;
	static thread_local FftPlanSse2 plan;
	plan.Prepare(n);
	int len = n;
	for (; len >= 8; len >>= 2) {
		int stage = 0;
		for (int x = len; x > 2; x >>= 1) stage++;
		const Cf* tw1 = &plan.twiddle[(size_t)plan.offset[(size_t)stage]];
		const Cf* tw2 = &plan.twiddle[(size_t)plan.offset[(size_t)(stage - 1)]];
		int quarter = len / 4;
		for (int base = 0; base < n; base += len) {
			int k = 0;
			for (; k + 1 < quarter; k += 2) {
				__m128 a = _mm_loadu_ps(reinterpret_cast<const float*>(
					data + base + k));
				__m128 b = _mm_loadu_ps(reinterpret_cast<const float*>(
					data + base + quarter + k));
				__m128 c = _mm_loadu_ps(reinterpret_cast<const float*>(
					data + base + 2 * quarter + k));
				__m128 d = _mm_loadu_ps(reinterpret_cast<const float*>(
					data + base + 3 * quarter + k));
				__m128 acSum = _mm_add_ps(a, c);
				__m128 bdSum = _mm_add_ps(b, d);
				__m128 acDiff = _mm_sub_ps(a, c);
				__m128 bdDiff = _mm_sub_ps(b, d);
				// -i * (b-d): [re,im] -> [im,-re].
				__m128 rotated = _mm_shuffle_ps(bdDiff, bdDiff,
					_MM_SHUFFLE(2, 3, 0, 1));
				rotated = _mm_xor_ps(rotated, _mm_castsi128_ps(_mm_setr_epi32(
					0, (int)0x80000000U, 0, (int)0x80000000U)));

				Cf w3[2];
				for (int q = 0; q < 2; q++) {
					int exponent = 3 * (k + q);
					bool negate = exponent >= len / 2;
					if (negate) exponent -= len / 2;
					w3[q] = negate
						? Cf(-tw1[exponent].re, -tw1[exponent].im)
						: tw1[exponent];
				}
				_mm_storeu_ps(reinterpret_cast<float*>(data + base + k),
					_mm_add_ps(acSum, bdSum));
				_mm_storeu_ps(reinterpret_cast<float*>(
					data + base + quarter + k), ComplexMulPair(
					_mm_sub_ps(acSum, bdSum), tw2 + k));
				_mm_storeu_ps(reinterpret_cast<float*>(
					data + base + 2 * quarter + k), ComplexMulPair(
					_mm_add_ps(acDiff, rotated), tw1 + k));
				_mm_storeu_ps(reinterpret_cast<float*>(
					data + base + 3 * quarter + k), ComplexMulPair(
					_mm_sub_ps(acDiff, rotated), w3));
			}
		}
	}
	if (len == 4) {
		for (int base = 0; base < n; base += 4) {
			Cf a = data[base], b = data[base + 1];
			Cf c = data[base + 2], d = data[base + 3];
			Cf acSum(a.re + c.re, a.im + c.im);
			Cf bdSum(b.re + d.re, b.im + d.im);
			Cf acDiff(a.re - c.re, a.im - c.im);
			Cf bdDiff(b.re - d.re, b.im - d.im);
			data[base] = Cf(acSum.re + bdSum.re, acSum.im + bdSum.im);
			data[base + 1] = Cf(acSum.re - bdSum.re, acSum.im - bdSum.im);
			data[base + 2] = Cf(acDiff.re + bdDiff.im,
				acDiff.im - bdDiff.re);
			data[base + 3] = Cf(acDiff.re - bdDiff.im,
				acDiff.im + bdDiff.re);
		}
	} else if (len == 2) {
		for (int base = 0; base < n; base += 2) {
			Cf a = data[base], b = data[base + 1];
			data[base] = Cf(a.re + b.re, a.im + b.im);
			data[base + 1] = Cf(a.re - b.re, a.im - b.im);
		}
	}
	return true;
}
#endif

// 4096-entry sine table shared by every Nco instance, built on first use.
// 12 bits of phase resolution is ~0.09 degrees, i.e. below the noise floor of
// anything this app receives, and it costs 16 KB - a quarter of this CPU's L1.
const int kSineBits = 12;
const int kSineSize = 1 << kSineBits;
float gSine[kSineSize];
bool gSineReady = false;

#if defined(__APPLE__) && defined(RSDR_USE_ACCELERATE)

// Keep the portable radix-2 implementation below for Haiku, but let macOS
// use the FFT implementation tuned for the current Apple CPU.  A split
// buffer is retained per DSP thread: DAB decoding and the UI spectrum can
// call Fft() concurrently, and neither should allocate for every symbol.
struct AccelerateFftState {
	FFTSetup setup;
	int log2n;
	std::vector<float> real;
	std::vector<float> imag;

	AccelerateFftState()
		:
		setup(NULL),
		log2n(-1)
	{
	}

	~AccelerateFftState()
	{
		if (setup != NULL)
			vDSP_destroy_fftsetup(setup);
	}

	bool Prepare(int n, int log2Size)
	{
		if (log2n != log2Size) {
			if (setup != NULL)
				vDSP_destroy_fftsetup(setup);
			setup = vDSP_create_fftsetup((vDSP_Length)log2Size,
				kFFTRadix2);
			log2n = setup != NULL ? log2Size : -1;
		}
		if (setup == NULL)
			return false;
		real.resize((size_t)n);
		imag.resize((size_t)n);
		return true;
	}
};

bool
AccelerateFft(Cf* data, int n)
{
	int log2n = 0;
	for (int size = n; size > 1 && (size & 1) == 0; size >>= 1)
		log2n++;
	if (n < 2 || ((size_t)1 << log2n) != (size_t)n)
		return false;

	static thread_local AccelerateFftState state;
	if (!state.Prepare(n, log2n))
		return false;

	// Manual interleaved/split conversion is inexpensive at these sizes and
	// avoids relying on vDSP_ctoz's specialised packed-real stride rules.
	for (int i = 0; i < n; i++) {
		state.real[(size_t)i] = data[i].re;
		state.imag[(size_t)i] = data[i].im;
	}
	DSPSplitComplex split = { &state.real[0], &state.imag[0] };
	vDSP_fft_zip(state.setup, &split, 1, (vDSP_Length)log2n,
		FFT_FORWARD);
	for (int i = 0; i < n; i++) {
		data[i].re = state.real[(size_t)i];
		data[i].im = state.imag[(size_t)i];
	}
	return true;
}

#endif

void
EnsureSineTable()
{
	if (gSineReady)
		return;
	for (int i = 0; i < kSineSize; i++)
		gSine[i] = sinf(2.0f * kPi * (float)i / (float)kSineSize);
	gSineReady = true;
}

inline float
TableSin(unsigned int phase)
{
	return gSine[phase >> (32 - kSineBits)];
}

inline float
TableCos(unsigned int phase)
{
	return gSine[((phase >> (32 - kSineBits)) + (kSineSize / 4))
		& (kSineSize - 1)];
}

} // namespace

float
FastAtan2(float y, float x)
{
	// Standard minimax cubic on |y/x| over one octant, then folded out to
	// the full circle. Max error ~1e-5 rad.
	if (x == 0.0f && y == 0.0f)
		return 0.0f;

	float ax = fabsf(x);
	float ay = fabsf(y);
	float a = (ax > ay) ? (ay / ax) : (ax / ay);
	float s = a * a;
	float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a
		+ a;

	if (ay > ax)
		r = 1.57079637f - r;
	if (x < 0.0f)
		r = 3.14159274f - r;
	if (y < 0.0f)
		r = -r;
	return r;
}

void
DesignLowpass(std::vector<float>& taps, int numTaps, float cutoffHz,
	float sampleRate)
{
	if (numTaps < 3)
		numTaps = 3;
	if ((numTaps % 2) == 0)
		numTaps++; // odd -> integer group delay

	taps.assign(numTaps, 0.0f);

	float fc = cutoffHz / sampleRate; // cycles/sample
	if (fc > 0.5f)
		fc = 0.5f;
	if (fc < 0.0f)
		fc = 0.0f;

	int mid = numTaps / 2;
	float sum = 0.0f;
	for (int i = 0; i < numTaps; i++) {
		int n = i - mid;
		float sinc;
		if (n == 0)
			sinc = 2.0f * fc;
		else {
			float x = 2.0f * kPi * fc * (float)n;
			sinc = sinf(x) / (kPi * (float)n);
		}
		// Hamming window: -43 dB sidelobes, which is enough to keep the
		// adjacent FM channel out and far cheaper to reason about than a
		// Parks-McClellan design generated offline.
		float w = 0.54f - 0.46f * cosf(2.0f * kPi * (float)i
			/ (float)(numTaps - 1));
		taps[i] = sinc * w;
		sum += taps[i];
	}

	// Unity DC gain, so a mode change cannot alter the audio level.
	if (sum != 0.0f) {
		for (int i = 0; i < numTaps; i++)
			taps[i] /= sum;
	}
}

void
Fft(Cf* data, int n)
{
	if (n < 2)
		return;

#if defined(__SSE2__) && !(defined(__APPLE__) && defined(RSDR_USE_ACCELERATE))
	if (FftSse2(data, n))
		return;
#endif

#if defined(__APPLE__) && defined(RSDR_USE_ACCELERATE)
	if (AccelerateFft(data, n))
		return;
#endif

	// Bit-reversal permutation.
	for (int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;
		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j) {
			Cf tmp = data[i];
			data[i] = data[j];
			data[j] = tmp;
		}
	}

	for (int len = 2; len <= n; len <<= 1) {
		float ang = -2.0f * kPi / (float)len;
		float wRe = cosf(ang);
		float wIm = sinf(ang);
		for (int i = 0; i < n; i += len) {
			float curRe = 1.0f;
			float curIm = 0.0f;
			for (int k = 0; k < len / 2; k++) {
				Cf u = data[i + k];
				Cf v = data[i + k + len / 2];
				float vRe = v.re * curRe - v.im * curIm;
				float vIm = v.re * curIm + v.im * curRe;
				data[i + k].re = u.re + vRe;
				data[i + k].im = u.im + vIm;
				data[i + k + len / 2].re = u.re - vRe;
				data[i + k + len / 2].im = u.im - vIm;
				float nextRe = curRe * wRe - curIm * wIm;
				curIm = curRe * wIm + curIm * wRe;
				curRe = nextRe;
			}
		}
	}
}

bool
FftBitReversed(Cf* data, int n)
{
#if defined(__SSE2__) && !(defined(__APPLE__) && defined(RSDR_USE_ACCELERATE))
	if (FftSse2BitReversed(data, n))
		return true;
#endif
	Fft(data, n);
	return false;
}

void
PowerSpectrumDb(const Cf* in, size_t inLen, int n, std::vector<float>& magsDb)
{
	static std::vector<Cf> work;
	work.assign((size_t)n, Cf());

	size_t use = inLen < (size_t)n ? inLen : (size_t)n;
	for (size_t i = 0; i < use; i++) {
		// Hann window: a rectangular window smears a strong FM carrier across
		// the whole display, which looks like a noise floor 30 dB too high.
		float w = 0.5f - 0.5f * cosf(2.0f * kPi * (float)i / (float)(n - 1));
		work[i].re = in[i].re * w;
		work[i].im = in[i].im * w;
	}

	Fft(&work[0], n);

	magsDb.assign((size_t)n, -140.0f);
	float norm = 1.0f / (float)n;
	for (int i = 0; i < n; i++) {
		float re = work[i].re * norm;
		float im = work[i].im * norm;
		float p = re * re + im * im;
		// fftshift: bin 0 belongs in the middle.
		int dst = (i + n / 2) % n;
		magsDb[dst] = 10.0f * log10f(p + 1e-14f);
	}
}

void
SoftClip(float* buf, size_t len, float threshold)
{
	if (threshold <= 0.0f || threshold >= 1.0f)
		return;
	const float span = 1.0f - threshold;
	for (size_t i = 0; i < len; i++) {
		float x = buf[i];
		float a = fabsf(x);
		if (a <= threshold)
			continue;
		float over = (a - threshold) / span;
		float y = threshold + span * tanhf(over);
		buf[i] = x < 0.0f ? -y : y;
	}
}

// #pragma mark - ComplexDecimator

ComplexDecimator::ComplexDecimator()
	:
	fDecim(1)
{
}

void
ComplexDecimator::Init(const std::vector<float>& taps, int decim)
{
	fTaps = taps;
	fDecim = decim > 0 ? decim : 1;
	Reset();
}

void
ComplexDecimator::Reset()
{
	// Priming with (numTaps - 1) zeros makes the first output correspond to
	// the filter's centre tap, so switching modes does not shift the audio.
	size_t prime = fTaps.empty() ? 0 : fTaps.size() - 1;
	fHist.assign(prime, Cf());
}

size_t
ComplexDecimator::MaxOutput(size_t inLen) const
{
	if (fTaps.empty())
		return inLen;
	return (inLen + fTaps.size()) / (size_t)fDecim + 1;
}

size_t
ComplexDecimator::Process(const Cf* in, size_t inLen, Cf* out)
{
	const size_t n = fTaps.size();
	if (n == 0) {
		memcpy(out, in, inLen * sizeof(Cf));
		return inLen;
	}

	fWork.resize(fHist.size() + inLen);
	if (!fHist.empty())
		memcpy(&fWork[0], &fHist[0], fHist.size() * sizeof(Cf));
	if (inLen > 0)
		memcpy(&fWork[fHist.size()], in, inLen * sizeof(Cf));

	const float* t = &fTaps[0];
	const Cf* w = &fWork[0];
	const size_t total = fWork.size();

	size_t pos = 0;
	size_t produced = 0;
	while (pos + n <= total) {
		const Cf* p = w + pos;
		float re = 0.0f;
		float im = 0.0f;
		for (size_t k = 0; k < n; k++) {
			re += p[k].re * t[k];
			im += p[k].im * t[k];
		}
		out[produced].re = re;
		out[produced].im = im;
		produced++;
		pos += (size_t)fDecim;
	}

	// The next window would start at fWork[pos]; keeping the tail from pos
	// onwards means the next call can always start its loop at index 0.
	// That tail is shorter than n by construction (the loop exited), so the
	// history never grows without bound.
	fHist.assign(fWork.begin() + pos, fWork.end());
	return produced;
}

// #pragma mark - RealDecimator

RealDecimator::RealDecimator()
	:
	fDecim(1)
{
}

void
RealDecimator::Init(const std::vector<float>& taps, int decim)
{
	fTaps = taps;
	fDecim = decim > 0 ? decim : 1;
	Reset();
}

void
RealDecimator::Reset()
{
	size_t prime = fTaps.empty() ? 0 : fTaps.size() - 1;
	fHist.assign(prime, 0.0f);
}

size_t
RealDecimator::MaxOutput(size_t inLen) const
{
	if (fTaps.empty())
		return inLen;
	return (inLen + fTaps.size()) / (size_t)fDecim + 1;
}

size_t
RealDecimator::Process(const float* in, size_t inLen, float* out)
{
	const size_t n = fTaps.size();
	if (n == 0) {
		memcpy(out, in, inLen * sizeof(float));
		return inLen;
	}

	fWork.resize(fHist.size() + inLen);
	if (!fHist.empty())
		memcpy(&fWork[0], &fHist[0], fHist.size() * sizeof(float));
	if (inLen > 0)
		memcpy(&fWork[fHist.size()], in, inLen * sizeof(float));

	const float* t = &fTaps[0];
	const float* w = &fWork[0];
	const size_t total = fWork.size();

	size_t pos = 0;
	size_t produced = 0;
	while (pos + n <= total) {
		const float* p = w + pos;
		float acc = 0.0f;
		for (size_t k = 0; k < n; k++)
			acc += p[k] * t[k];
		out[produced++] = acc;
		pos += (size_t)fDecim;
	}

	fHist.assign(fWork.begin() + pos, fWork.end());
	return produced;
}

// #pragma mark - ComplexBandpass

ComplexBandpass::ComplexBandpass()
{
}

void
ComplexBandpass::Init(int numTaps, float lowHz, float highHz, float sampleRate)
{
	if (highHz < lowHz) {
		float tmp = highHz;
		highHz = lowHz;
		lowHz = tmp;
	}

	// A real lowpass of half the passband width, frequency-shifted to the
	// passband centre. The result has taps h[n] * exp(j*2*pi*fc*n), which
	// passes one side of the spectrum and rejects the mirror image - the
	// whole point for SSB.
	float centre = 0.5f * (lowHz + highHz);
	float halfWidth = 0.5f * (highHz - lowHz);

	std::vector<float> proto;
	DesignLowpass(proto, numTaps, halfWidth, sampleRate);

	fTaps.assign(proto.size(), Cf());
	int mid = (int)proto.size() / 2;
	for (size_t i = 0; i < proto.size(); i++) {
		// Note the minus sign. Process() evaluates a *correlation* - it walks
		// the taps forwards over a forwards window - which is the same as
		// convolving with the taps reversed. For the symmetric real filters
		// elsewhere in this file that makes no difference, but reversing a
		// complex tap array conjugates its frequency response and moves the
		// passband from +centre to -centre. Building the taps with the
		// opposite phase cancels that out.
		//
		// Getting this wrong is not subtle in the end result and is
		// completely invisible in the code: LSB and USB simply swap, and
		// every test that only measures output level still passes, because
		// the AGC brings the rejected sideband's noise back up to the same
		// loudness.
		float phase = -2.0f * kPi * centre * (float)((int)i - mid) / sampleRate;
		fTaps[i].re = proto[i] * cosf(phase);
		fTaps[i].im = proto[i] * sinf(phase);
	}
	Reset();
}

void
ComplexBandpass::Reset()
{
	size_t prime = fTaps.empty() ? 0 : fTaps.size() - 1;
	fHist.assign(prime, Cf());
}

void
ComplexBandpass::Process(const Cf* in, size_t inLen, Cf* out)
{
	const size_t n = fTaps.size();
	if (n == 0) {
		memcpy(out, in, inLen * sizeof(Cf));
		return;
	}

	fWork.resize(fHist.size() + inLen);
	if (!fHist.empty())
		memcpy(&fWork[0], &fHist[0], fHist.size() * sizeof(Cf));
	if (inLen > 0)
		memcpy(&fWork[fHist.size()], in, inLen * sizeof(Cf));

	const Cf* t = &fTaps[0];
	const Cf* w = &fWork[0];
	for (size_t i = 0; i < inLen; i++) {
		const Cf* p = w + i;
		float re = 0.0f;
		float im = 0.0f;
		for (size_t k = 0; k < n; k++) {
			re += p[k].re * t[k].re - p[k].im * t[k].im;
			im += p[k].re * t[k].im + p[k].im * t[k].re;
		}
		out[i].re = re;
		out[i].im = im;
	}

	fHist.assign(fWork.end() - (n - 1), fWork.end());
}

// #pragma mark - Nco

Nco::Nco()
	:
	fPhase(0),
	fStep(0)
{
	EnsureSineTable();
}

void
Nco::SetFrequency(float hz, float sampleRate)
{
	EnsureSineTable();
	if (sampleRate <= 0.0f) {
		fStep = 0;
		return;
	}
	// Wrapping the 32-bit accumulator is exactly modulo 2*pi, so any
	// frequency is represented to within 2^-32 of the sample rate and the
	// phase never accumulates error.
	double frac = (double)hz / (double)sampleRate;
	frac -= (double)(long long)frac;
	fStep = (unsigned int)(long long)(frac * 4294967296.0);
}

void
Nco::Mix(const Cf* in, size_t len, Cf* out)
{
	if (fStep == 0) {
		if (in != out)
			memcpy(out, in, len * sizeof(Cf));
		return;
	}
	unsigned int phase = fPhase;
	// The DAB front-end mixes a 2048-sample symbol for every FFT.  Keep the
	// sine-table lookups scalar (SSE2 has no gather), but do the four complex
	// multiplies together.  Loads happen before stores, so this is safe when
	// in == out just like the scalar path.
#if defined(__SSE2__)
	for (size_t i = 0; i + 4 <= len; i += 4) {
		float c[4], s[4];
		for (int j = 0; j < 4; j++) {
			c[j] = TableCos(phase);
			s[j] = TableSin(phase);
			phase += fStep;
		}
		const float* src = reinterpret_cast<const float*>(in + i);
		__m128 x0 = _mm_loadu_ps(src);
		__m128 x1 = _mm_loadu_ps(src + 4);
		__m128 xr = _mm_shuffle_ps(x0, x1, _MM_SHUFFLE(2, 0, 2, 0));
		__m128 xi = _mm_shuffle_ps(x0, x1, _MM_SHUFFLE(3, 1, 3, 1));
		__m128 cv = _mm_loadu_ps(c);
		__m128 sv = _mm_loadu_ps(s);
		__m128 rr = _mm_sub_ps(_mm_mul_ps(xr, cv), _mm_mul_ps(xi, sv));
		__m128 ri = _mm_add_ps(_mm_mul_ps(xr, sv), _mm_mul_ps(xi, cv));
		float* dst = reinterpret_cast<float*>(out + i);
		_mm_storeu_ps(dst, _mm_unpacklo_ps(rr, ri));
		_mm_storeu_ps(dst + 4, _mm_unpackhi_ps(rr, ri));
	}
	for (size_t i = len & ~(size_t)3; i < len; i++) {
		float c = TableCos(phase);
		float s = TableSin(phase);
		float re = in[i].re * c - in[i].im * s;
		float im = in[i].re * s + in[i].im * c;
		out[i].re = re;
		out[i].im = im;
		phase += fStep;
	}
#else
	for (size_t i = 0; i < len; i++) {
		float c = TableCos(phase);
		float s = TableSin(phase);
		float re = in[i].re * c - in[i].im * s;
		float im = in[i].re * s + in[i].im * c;
		out[i].re = re;
		out[i].im = im;
		phase += fStep;
	}
#endif
	fPhase = phase;
}

// #pragma mark - DcBlocker

void
DcBlocker::Process(float* buf, size_t len)
{
	// y[n] = x[n] - x[n-1] + 0.999 * y[n-1]; corner is about 7 Hz at 44.1 kHz,
	// well below the lowest audio content and well above any drift rate.
	float prevIn = fPrevIn;
	float prevOut = fPrevOut;
	for (size_t i = 0; i < len; i++) {
		float x = buf[i];
		float y = x - prevIn + 0.999f * prevOut;
		prevIn = x;
		prevOut = y;
		buf[i] = y;
	}
	fPrevIn = prevIn;
	fPrevOut = prevOut;
}

// #pragma mark - Deemphasis

void
Deemphasis::Init(float tauSeconds, float sampleRate)
{
	if (tauSeconds <= 0.0f || sampleRate <= 0.0f) {
		fAlpha = 1.0f;
		return;
	}
	fAlpha = 1.0f - expf(-1.0f / (sampleRate * tauSeconds));
	fState = 0.0f;
}

void
Deemphasis::Process(float* buf, size_t len)
{
	float a = fAlpha;
	float y = fState;
	for (size_t i = 0; i < len; i++) {
		y += a * (buf[i] - y);
		buf[i] = y;
	}
	fState = y;
}

// #pragma mark - Agc

Agc::Agc()
	:
	fTarget(0.3f),
	fAttack(0.01f),
	fDecay(0.0005f),
	fEnvelope(0.0f),
	fGain(1.0f)
{
}

void
Agc::Init(float sampleRate, float target)
{
	fTarget = target;
	// ~2 ms attack, ~600 ms decay.
	fAttack = 1.0f - expf(-1.0f / (sampleRate * 0.002f));
	fDecay = 1.0f - expf(-1.0f / (sampleRate * 0.6f));
	Reset();
}

void
Agc::Reset()
{
	// Negative means "not seeded yet". Starting from zero instead makes the
	// gain start at its maximum and produce a loud crack on the first
	// hundred samples after every mode change - the DC blocker ahead of it
	// is still settling at that moment, so there is a real step to amplify.
	fEnvelope = -1.0f;
	fGain = 1.0f;
}

void
Agc::Process(float* buf, size_t len)
{
	if (len == 0)
		return;

	float env = fEnvelope;
	if (env < 0.0f)
		env = fabsf(buf[0]);

	for (size_t i = 0; i < len; i++) {
		float mag = fabsf(buf[i]);
		// Asymmetric one-pole envelope follower.
		env += (mag > env ? fAttack : fDecay) * (mag - env);
		// The floor stops the gain running away to +80 dB during a silent
		// passage and then clipping on the first syllable back.
		float gain = env > 1e-4f ? fTarget / env : fTarget / 1e-4f;
		if (gain > 60.0f)
			gain = 60.0f;
		float v = buf[i] * gain;
		// Hard limit: a step that outruns the 2 ms attack still has to not
		// leave the sound card's range.
		if (v > 0.99f)
			v = 0.99f;
		else if (v < -0.99f)
			v = -0.99f;
		buf[i] = v;
		fGain = gain;
	}
	fEnvelope = env;
}

} // namespace dsp
