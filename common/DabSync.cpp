#include "DabSync.h"

#include <cmath>
#include <cstring>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

#if defined(__APPLE__) && defined(__aarch64__) \
	&& defined(RSDR_USE_ACCELERATE)
#include <arm_neon.h>
#endif

namespace {

const float kTwoPi = 6.28318530717958647692f;

}

DabSync::DabSync()
	:
	fBuffer(NULL),
	fCount(0),
	fSpecValid(false),
	fSkipFic(false)
{
	_BuildMapper();
	fScratch.resize(kUsefulSamples);
	fCur.resize(kCarriers);
	fPrev.resize(kCarriers);
	fBest.resize(kCarriers);
	fNominal.resize(kCarriers);
}

void
DabSync::SetBuffer(const dsp::Cf* x, size_t count)
{
	fBuffer = x;
	fCount = count;
}

void
DabSync::_BuildMapper()
{
	// EN 300 401 clause 14.6, generated rather than tabulated.
	std::vector<int> tmp((size_t)kUsefulSamples);
	tmp[0] = 0;
	for (int i = 1; i < kUsefulSamples; i++)
		tmp[i] = (13 * tmp[i - 1] + 511) % kUsefulSamples;

	fMapper.clear();
	fMapperBitReversed.clear();
	fMapper.reserve(kCarriers);
	fMapperBitReversed.reserve(kCarriers);
	const int half = kUsefulSamples / 2;
	for (int i = 0; i < kUsefulSamples; i++) {
		int t = tmp[i];
		if (t == half)
			continue;
		if (t < 256 || t > 256 + kCarriers)
			continue;
		int carrier = t - half;
		int bin = carrier < 0 ? carrier + kUsefulSamples : carrier;
		fMapper.push_back(bin);
		int x = bin;
		int reversed = 0;
		for (int bit = 1; bit < kUsefulSamples; bit <<= 1) {
			reversed = (reversed << 1) | (x & 1);
			x >>= 1;
		}
		fMapperBitReversed.push_back(reversed);
	}
}

bool
DabSync::_Spectrum(int pos, float fineHz, dsp::Cf* out)
{
	int o = pos + kGuardSamples;
	if (o < 0 || (size_t)(o + kUsefulSamples) > fCount)
		return false;

	dsp::Nco nco;
	nco.SetFrequency(-fineHz, (float)kSampleRate);
	// Phase must follow the absolute sample index. Resetting per symbol leaves
	// a constant rotation between symbols, which differential demodulation
	// turns into a rotated constellation that MER cannot see.
	nco.SetPhaseForSample((unsigned int)o);
	nco.Mix(fBuffer + o, kUsefulSamples, &fScratch[0]);

	bool bitReversed = dsp::FftBitReversed(&fScratch[0], kUsefulSamples);
	const float scale = 1.0f / sqrtf((float)kUsefulSamples);
	for (int k = 0; k < kCarriers; k++) {
		int bin = bitReversed ? fMapperBitReversed[k] : fMapper[k];
		const dsp::Cf& v = fScratch[bin];
		out[k].re = v.re * scale;
		out[k].im = v.im * scale;
	}
	return true;
}

float
DabSync::_Quality(const dsp::Cf* cur, const dsp::Cf* prev) const
{
	// pi/4-DQPSK: the normalised differential product sits at 45 + k*90
	// degrees, so u^4 must be -1. No symbol decisions needed, and it
	// vectorises - but see the header for what it cannot distinguish.
	double sum = 0.0;
	int measured = 0;
#if defined(__SSE2__)
	// Four adjacent carriers every sixteen span the entire occupied band while
	// cutting this drop detector to one quarter of its former work. Timing
	// shifts rotate carriers as a slope across the band, so contiguous coverage
	// is not required; broad frequency coverage is.
	for (int k = 0; k + 4 <= kCarriers; k += 16) {
		__m128 c0 = _mm_loadu_ps(reinterpret_cast<const float*>(cur + k));
		__m128 c1 = _mm_loadu_ps(reinterpret_cast<const float*>(cur + k + 2));
		__m128 p0 = _mm_loadu_ps(reinterpret_cast<const float*>(prev + k));
		__m128 p1 = _mm_loadu_ps(reinterpret_cast<const float*>(prev + k + 2));
		__m128 cr = _mm_shuffle_ps(c0, c1, _MM_SHUFFLE(2, 0, 2, 0));
		__m128 ci = _mm_shuffle_ps(c0, c1, _MM_SHUFFLE(3, 1, 3, 1));
		__m128 pr = _mm_shuffle_ps(p0, p1, _MM_SHUFFLE(2, 0, 2, 0));
		__m128 pi = _mm_shuffle_ps(p0, p1, _MM_SHUFFLE(3, 1, 3, 1));
		__m128 re = _mm_add_ps(_mm_mul_ps(cr, pr), _mm_mul_ps(ci, pi));
		__m128 im = _mm_sub_ps(_mm_mul_ps(ci, pr), _mm_mul_ps(cr, pi));
		__m128 n = _mm_add_ps(_mm_mul_ps(re, re), _mm_mul_ps(im, im));
		__m128 r2 = _mm_sub_ps(_mm_mul_ps(re, re), _mm_mul_ps(im, im));
		__m128 i2 = _mm_add_ps(_mm_mul_ps(re, im), _mm_mul_ps(re, im));
		__m128 r4 = _mm_sub_ps(_mm_mul_ps(r2, r2), _mm_mul_ps(i2, i2));
		__m128 i4 = _mm_add_ps(_mm_mul_ps(r2, i2), _mm_mul_ps(r2, i2));
		__m128 inv = _mm_div_ps(_mm_set1_ps(1.0f), _mm_mul_ps(n, n));
		__m128 dr = _mm_add_ps(_mm_mul_ps(r4, inv), _mm_set1_ps(1.0f));
		__m128 di = _mm_mul_ps(i4, inv);
		__m128 v = _mm_mul_ps(dr, dr);
		v = _mm_add_ps(v, _mm_mul_ps(di, di));
		float lanes[4];
		_mm_storeu_ps(lanes, v);
		sum += lanes[0] + lanes[1] + lanes[2] + lanes[3];
		measured += 4;
	}
#else
	for (int k = 0; k < kCarriers; k++) {
		measured++;
		float re = cur[k].re * prev[k].re + cur[k].im * prev[k].im;
		float im = cur[k].im * prev[k].re - cur[k].re * prev[k].im;
		// Raise to the fourth and divide by |q|^4 at the end, rather than
		// normalising first: same value, one divide instead of a square root
		// per carrier, and this runs 1536 times per candidate position.
		float n = re * re + im * im;
		if (n < 1e-24f)
			continue;
		float r2 = re * re - im * im;
		float i2 = 2.0f * re * im;
		float r4 = r2 * r2 - i2 * i2;
		float i4 = 2.0f * r2 * i2;
		float inv = 1.0f / (n * n);
		float dr = r4 * inv + 1.0f;
		float di = i4 * inv;
		sum += (double)(dr * dr + di * di);
	}
#endif
	double mean = sum / (double)measured;
	if (mean < 1e-12)
		mean = 1e-12;
	return (float)(-10.0 * log10(mean));
}

int
DabSync::_Timing(int start, int search) const
{
	const int kSymbols = 12;
	float bestMag = -1.0f;
	int bestShift = 0;
	for (int shift = -search; shift < search; shift++) {
		int base = start + shift;
		if (base < 0)
			continue;
		if ((size_t)(base + kSymbols * kSymbolSamples + kUsefulSamples
				+ kGuardSamples) > fCount) {
			continue;
		}
		float re = 0.0f;
		float im = 0.0f;
		for (int s = 0; s < kSymbols; s++) {
			const dsp::Cf* g = fBuffer + base + s * kSymbolSamples;
			const dsp::Cf* t = g + kUsefulSamples;
			for (int k = 0; k < kGuardSamples; k++) {
				re += t[k].re * g[k].re + t[k].im * g[k].im;
				im += t[k].re * g[k].im - t[k].im * g[k].re;
			}
		}
		float mag = re * re + im * im;
		if (mag > bestMag) {
			bestMag = mag;
			bestShift = shift;
		}
	}
	return bestShift;
}

int
DabSync::_NextTiming(int expected) const
{
	const int kSymbols = 12;
	float bestMag = -1.0f;
	int best = -1;
	for (int drop = 0; drop <= kMaxDropSteps; drop++) {
		for (int jitter = -2; jitter <= 2; jitter++) {
			int base = expected - drop * kDropQuantum + jitter;
			if (base < 0 || (size_t)(base + kSymbols * kSymbolSamples) > fCount) {
				continue;
			}
			float re = 0.0f;
			float im = 0.0f;
			for (int s = 0; s < kSymbols; s++) {
				const dsp::Cf* g = fBuffer + base + s * kSymbolSamples;
				const dsp::Cf* t = g + kUsefulSamples;
				for (int k = 0; k < kGuardSamples; k++) {
					re += t[k].re * g[k].re + t[k].im * g[k].im;
					im += t[k].re * g[k].im - t[k].im * g[k].re;
				}
			}
			float mag = re * re + im * im;
			if (mag > bestMag) {
				bestMag = mag;
				best = base;
			}
		}
	}
	return best;
}

int
DabSync::NextFrame(int expected, float* fineHz)
{
	if (fBuffer == NULL)
		return -1;
	int start = _NextTiming(expected);
	if (start < 0)
		return -1;
	// _NextTiming() only needs twelve symbols, while the fine-frequency
	// estimate prefers forty.  A frame close to the end of a capture block
	// may have fewer than forty available; never let _FineFrequency() walk
	// past fBuffer in that case.
	int available = (int)fCount - start - kUsefulSamples;
	int symbols = available / kSymbolSamples;
	if (symbols > 40)
		symbols = 40;
	if (symbols < 4)
		return -1;
	if (fineHz != NULL)
		*fineHz = _FineFrequency(start, symbols);
	return start;
}

float
DabSync::_FineFrequency(int start, int symbols) const
{
	float re = 0.0f;
	float im = 0.0f;
	for (int s = 0; s < symbols; s++) {
		const dsp::Cf* g = fBuffer + start + s * kSymbolSamples;
		const dsp::Cf* t = g + kUsefulSamples;
		for (int k = 0; k < kGuardSamples; k++) {
			re += t[k].re * g[k].re + t[k].im * g[k].im;
			im += t[k].re * g[k].im - t[k].im * g[k].re;
		}
	}
	float phase = dsp::FastAtan2(im, re);
	return -phase / (kTwoPi * (float)kUsefulSamples / (float)kSampleRate);
}

int
DabSync::FindFrame(size_t from, float* fineHz, float* nullDepth)
{
	if (fBuffer == NULL)
		return -1;
	size_t needed = (size_t)kNullSamples + (size_t)kFrameSamples;
	if (fCount < from + needed)
		return -1;

	size_t searchEnd = from + kFrameSamples;
	if (searchEnd + kNullSamples > fCount)
		searchEnd = fCount - kNullSamples;

	// Sliding power over the null length; the deepest window is the null.
	double window = 0.0;
	for (int i = 0; i < kNullSamples; i++) {
		const dsp::Cf& v = fBuffer[from + i];
		window += (double)v.re * v.re + (double)v.im * v.im;
	}
	double total = window;
	double best = window;
	size_t nullPos = from;
	size_t counted = kNullSamples;
	for (size_t i = from + 1; i < searchEnd; i++) {
		const dsp::Cf& out = fBuffer[i - 1];
		const dsp::Cf& in = fBuffer[i + kNullSamples - 1];
		window -= (double)out.re * out.re + (double)out.im * out.im;
		window += (double)in.re * in.re + (double)in.im * in.im;
		total += (double)in.re * in.re + (double)in.im * in.im;
		counted++;
		if (window < best) {
			best = window;
			nullPos = i;
		}
	}
	double meanPower = total / (double)counted;
	if (meanPower <= 0.0)
		return -1;
	float depth = (float)(best / (double)kNullSamples / meanPower);
	if (nullDepth != NULL)
		*nullDepth = depth;
	if (depth > 0.35f)
		return -1;

	int firstSymbol = (int)nullPos + kNullSamples;
	int shift = _Timing(firstSymbol, 320);
	int start = firstSymbol + shift;
	if (start < 0)
		return -1;

	int available = (int)fCount - start - kUsefulSamples - kGuardSamples;
	int symbols = available / kSymbolSamples;
	if (symbols > 40)
		symbols = 40;
	if (symbols < 4)
		return -1;
	if (fineHz != NULL)
		*fineHz = _FineFrequency(start, symbols);
	return start;
}

bool
DabSync::TrackFrame(int start, float fineHz, int* positions)
{
	if (fBuffer == NULL)
		return false;
	fSpec.resize((size_t)kSymbolsPerFrame * kCarriers);
	if (!_Spectrum(start, fineHz, &fSpec[0]))
		return false;
	positions[0] = start;
	fSpecValid = false;
	int firstSymbol = 1;
	if (fSkipFic) {
		// FIC symbols 1 and 2 are not needed after service selection. Keep
		// their nominal positions for frame walking, then calculate symbol 3
		// as the reference for the first MSC differential symbol.
		positions[1] = positions[0] + kSymbolSamples;
		positions[2] = positions[1] + kSymbolSamples;
		positions[3] = positions[2] + kSymbolSamples;
		if (!_Spectrum(positions[3], fineHz,
				&fSpec[(size_t)3 * kCarriers]))
			return false;
		firstSymbol = 4;
	}

	// Most symbols follow the previous one at the nominal spacing, so try that
	// first and only pay for the 75-position search when the quality says a
	// drop happened. Drops were measured at two to four per frame.
	//
	// The accept threshold cannot be a constant. This metric reads about 12 dB
	// below MER (it squares the error twice), so a signal at a perfectly good
	// 15 dB scores around 3 - and a fixed threshold of 10 sent every single
	// symbol down the slow path, which cost 52x real time. So it calibrates
	// itself against what this signal actually achieves.
	float accept = -1e9f;
	float ema = 0.0f;
	bool haveEma = false;
	int searches = 0;

	for (int s = firstSymbol; s < kSymbolsPerFrame; s++) {
		int base = positions[s - 1] + kSymbolSamples;
		const dsp::Cf* prev = &fSpec[(size_t)(s - 1) * kCarriers];
		int bestPos = -1;
		float bestQ = -1e9f;
		float nominalQ = -1e9f;
		int budget = kSearchesPerFrame;

		if (_Spectrum(base, fineHz, &fCur[0])) {
			// Once the per-frame search budget is spent, the old path still
			// evaluated the 1536-carrier quality metric only to immediately
			// accept the nominal position. Keep the already computed FFT and
			// skip that metric; this is the common path after the first drop.
			if (searches >= budget) {
				memcpy(&fSpec[(size_t)s * kCarriers], &fCur[0],
					kCarriers * sizeof(dsp::Cf));
				positions[s] = base;
				continue;
			}
			float q = _Quality(&fCur[0], prev);
			nominalQ = q;
			// 4.5 dB rather than 3: on a weak signal the metric is noisy
			// enough that a 3 dB margin fires on most symbols, and every one
			// of those false alarms costs a dozen FFTs.
			if (q >= accept || searches >= budget) {
				memcpy(&fSpec[(size_t)s * kCarriers], &fCur[0],
					kCarriers * sizeof(dsp::Cf));
				positions[s] = base;
				ema = haveEma ? 0.9f * ema + 0.1f * q : q;
				haveEma = true;
				accept = ema - 4.5f;
				continue;
			}
			bestQ = q;
			bestPos = base;
			memcpy(&fBest[0], &fCur[0], kCarriers * sizeof(dsp::Cf));
			// Keep it: if the search finds nothing better we fall back here,
			// and recomputing this FFT would be pure waste.
			memcpy(&fNominal[0], &fCur[0], kCarriers * sizeof(dsp::Cf));
		}
		searches++;

		// First choose the measured 94-sample drop multiple, then refine only
		// that candidate by +/-2 samples. The old Cartesian search performed
		// 74 extra 2048-point FFTs for every drop; this performs at most 18 and
		// gives the same positions on the live captures.
		int bestDrop = 0;
		for (int k = 1; k <= kMaxDropSteps; k++) {
			int p = base - k * kDropQuantum;
			if (!_Spectrum(p, fineHz, &fCur[0]))
				continue;
			float q = _Quality(&fCur[0], prev);
			if (q > bestQ) {
				bestQ = q;
				bestPos = p;
				bestDrop = k;
				memcpy(&fBest[0], &fCur[0], kCarriers * sizeof(dsp::Cf));
			}
		}
		for (int j = -2; j <= 2; j++) {
			if (j == 0)
				continue;
			int p = base - bestDrop * kDropQuantum + j;
			if (!_Spectrum(p, fineHz, &fCur[0]))
				continue;
			float q = _Quality(&fCur[0], prev);
			if (q > bestQ) {
				bestQ = q;
				bestPos = p;
				memcpy(&fBest[0], &fCur[0], kCarriers * sizeof(dsp::Cf));
			}
		}
		if (bestPos < 0)
			return false;
		// Only move the grid if the shifted position is clearly better. A drop
		// is worth several dB; anything smaller is the metric chasing noise,
		// and moving on that basis makes the next symbol look wrong too.
		if (bestPos != base && bestQ < nominalQ + 1.0f) {
			bestPos = base;
			bestQ = nominalQ;
			memcpy(&fBest[0], &fNominal[0], kCarriers * sizeof(dsp::Cf));
		}
		positions[s] = bestPos;
		memcpy(&fSpec[(size_t)s * kCarriers], &fBest[0],
			kCarriers * sizeof(dsp::Cf));
		ema = haveEma ? 0.9f * ema + 0.1f * bestQ : bestQ;
		haveEma = true;
		accept = ema - 4.5f;
	}
	fSpecValid = true;
	return true;
}

void
DabSync::SoftBits(const int* positions, float fineHz, float* out)
{
	// Reuse what tracking already computed; only fall back to FFTs if the
	// caller asks for positions this object did not just track.
	if (!fSpecValid) {
		fSpec.resize((size_t)kSymbolsPerFrame * kCarriers);
		for (int s = 0; s < kSymbolsPerFrame; s++) {
			if (!_Spectrum(positions[s], fineHz,
					&fSpec[(size_t)s * kCarriers])) {
				return;
			}
		}
		fSpecValid = true;
	}

	int first = fSkipFic ? 4 : 1;
	for (int s = first; s < kSymbolsPerFrame; s++) {
		const dsp::Cf* cur = &fSpec[(size_t)s * kCarriers];
		const dsp::Cf* prev = &fSpec[(size_t)(s - 1) * kCarriers];
		float* dst = out + (size_t)(s - 1) * kSoftPerSymbol;
		int k = 0;
#if defined(__APPLE__) && defined(__aarch64__) \
	&& defined(RSDR_USE_ACCELERATE)
		// Four complex carriers per iteration. vld2 deinterleaves the POD
		// {re, im} layout directly, while the two output components are already
		// contiguous in the DAB soft-bit format.
		const float32x4_t minimum = vdupq_n_f32(1e-12f);
		for (; k + 4 <= kCarriers; k += 4) {
			float32x4x2_t c = vld2q_f32(
				reinterpret_cast<const float*>(cur + k));
			float32x4x2_t p = vld2q_f32(
				reinterpret_cast<const float*>(prev + k));
			float32x4_t re = vaddq_f32(vmulq_f32(c.val[0], p.val[0]),
				vmulq_f32(c.val[1], p.val[1]));
			float32x4_t im = vsubq_f32(vmulq_f32(c.val[1], p.val[0]),
				vmulq_f32(c.val[0], p.val[1]));
			float32x4_t magnitude = vsqrtq_f32(vaddq_f32(
				vmulq_f32(re, re), vmulq_f32(im, im)));
			magnitude = vmaxq_f32(magnitude, minimum);
			vst1q_f32(dst + k, vdivq_f32(re, magnitude));
			vst1q_f32(dst + kCarriers + k,
				vdivq_f32(im, magnitude));
		}
#elif defined(__SSE2__)
		// Four complex carriers per iteration, the x86 mirror of the NEON path.
		// SSE2 has no vld2, so deinterleave the {re, im} pairs with shuffles.
		const __m128 minimumPower = _mm_set1_ps(1e-24f);
		for (; k + 4 <= kCarriers; k += 4) {
			const float* cp = reinterpret_cast<const float*>(cur + k);
			const float* pp = reinterpret_cast<const float*>(prev + k);
			__m128 c0 = _mm_loadu_ps(cp);
			__m128 c1 = _mm_loadu_ps(cp + 4);
			__m128 p0 = _mm_loadu_ps(pp);
			__m128 p1 = _mm_loadu_ps(pp + 4);
			__m128 cre = _mm_shuffle_ps(c0, c1, _MM_SHUFFLE(2, 0, 2, 0));
			__m128 cim = _mm_shuffle_ps(c0, c1, _MM_SHUFFLE(3, 1, 3, 1));
			__m128 pre = _mm_shuffle_ps(p0, p1, _MM_SHUFFLE(2, 0, 2, 0));
			__m128 pim = _mm_shuffle_ps(p0, p1, _MM_SHUFFLE(3, 1, 3, 1));
			__m128 re = _mm_add_ps(_mm_mul_ps(cre, pre), _mm_mul_ps(cim, pim));
			__m128 im = _mm_sub_ps(_mm_mul_ps(cim, pre), _mm_mul_ps(cre, pim));
			__m128 power = _mm_max_ps(_mm_add_ps(_mm_mul_ps(re, re),
				_mm_mul_ps(im, im)), minimumPower);
			// Direction, rather than absolute magnitude, carries the DQPSK soft
			// decision. SSE's reciprocal-square-root estimate has ample precision
			// for that direction (relative error below the RF noise) and avoids one
			// square root plus two divides per four carriers on the Atom.
			__m128 invMag = _mm_rsqrt_ps(power);
			_mm_storeu_ps(dst + k, _mm_mul_ps(re, invMag));
			_mm_storeu_ps(dst + kCarriers + k, _mm_mul_ps(im, invMag));
		}
#endif
		for (; k < kCarriers; k++) {
			float re = cur[k].re * prev[k].re + cur[k].im * prev[k].im;
			float im = cur[k].im * prev[k].re - cur[k].re * prev[k].im;
			float m = sqrtf(re * re + im * im);
			if (m < 1e-12f)
				m = 1e-12f;
			dst[k] = re / m;
			dst[kCarriers + k] = im / m;
		}
	}
}
