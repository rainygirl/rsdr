#include "DabProbe.h"

#include <OS.h>

#include <cmath>
#include <cstring>

namespace {

const float kTwoPi = 6.28318530717958647692f;

// One analysis a second. Each one costs a few tens of MFLOP; running it on
// every frame would be ten times that for no extra information.
const bigtime_t kAnalysisInterval = 1000000;

} // namespace

DabProbe::DabProbe()
	:
	fLastAnalysis(0)
{
	fSymbol.resize(kUsefulSamples);
	_BuildMapper();
	fFicSoft.resize((size_t)3 * 4 * kCarriersPerSide);   // 3 x 3072
}

void
DabProbe::_BuildMapper()
{
	// EN 300 401 clause 14.6. The permutation is generated, not tabulated:
	// t[0] = 0, t[i] = (13 * t[i-1] + 511) mod 2048, keeping the values that
	// land on an active carrier and shifting them to a signed carrier index.
	std::vector<int> tmp((size_t)kUsefulSamples);
	tmp[0] = 0;
	for (int i = 1; i < kUsefulSamples; i++)
		tmp[i] = (13 * tmp[i - 1] + 511) % kUsefulSamples;

	fMapper.clear();
	fMapper.reserve(2 * kCarriersPerSide);
	const int half = kUsefulSamples / 2;
	const int k = 2 * kCarriersPerSide;
	for (int i = 0; i < kUsefulSamples; i++) {
		int t = tmp[i];
		if (t == half)
			continue;
		if (t < 256 || t > 256 + k)
			continue;
		int carrier = t - half;
		// Store the FFT bin directly; negative frequencies live in the upper
		// half of a natural-order FFT.
		fMapper.push_back(carrier < 0 ? carrier + kUsefulSamples : carrier);
	}
}

void
DabProbe::Reset()
{
	fResult = result();
	fFic.Reset();
	fLastAnalysis = 0;
	fWindow.clear();
}

void
DabProbe::Feed(const uint8* iq, size_t bytes)
{
	bigtime_t now = system_time();
	if (fLastAnalysis != 0 && now - fLastAnalysis < kAnalysisInterval)
		return;

	size_t samples = bytes / 2;
	if (samples < (size_t)kWindowSamples)
		return;		// see the header: one block must hold a whole frame

	fLastAnalysis = now;

	fWindow.resize((size_t)kWindowSamples);
	const float scale = 1.0f / 127.5f;
	for (int i = 0; i < kWindowSamples; i++) {
		fWindow[i].re = ((float)iq[2 * i] - 127.5f) * scale;
		fWindow[i].im = ((float)iq[2 * i + 1] - 127.5f) * scale;
	}

	// The RTL2832 has a real DC offset, and it sits right on carrier 0.
	double sumRe = 0.0;
	double sumIm = 0.0;
	for (int i = 0; i < kWindowSamples; i++) {
		sumRe += fWindow[i].re;
		sumIm += fWindow[i].im;
	}
	float meanRe = (float)(sumRe / kWindowSamples);
	float meanIm = (float)(sumIm / kWindowSamples);
	for (int i = 0; i < kWindowSamples; i++) {
		fWindow[i].re -= meanRe;
		fWindow[i].im -= meanIm;
	}

	_Analyze(&fWindow[0], (size_t)kWindowSamples);
}

int
DabProbe::_Timing(const dsp::Cf* x, size_t count, int start, int search) const
{
	// Correlate each symbol's guard against the tail it is a copy of, summed
	// coherently over several symbols, and take the alignment that maximises
	// it. This is what pins the symbol boundary to the sample.
	const int kSymbols = 12;
	float bestMag = -1.0f;
	int bestShift = 0;

	for (int shift = -search; shift < search; shift++) {
		int base = start + shift;
		if (base < 0)
			continue;
		if ((size_t)(base + kSymbols * kSymbolSamples + kUsefulSamples
				+ kGuardSamples) > count) {
			continue;
		}

		float re = 0.0f;
		float im = 0.0f;
		for (int s = 0; s < kSymbols; s++) {
			const dsp::Cf* g = x + base + s * kSymbolSamples;
			const dsp::Cf* t = g + kUsefulSamples;
			for (int k = 0; k < kGuardSamples; k++) {
				// conj(tail) * guard
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

float
DabProbe::_FineFrequency(const dsp::Cf* x, int start, int symbols) const
{
	// A carrier offset makes the guard and its copy differ in phase by
	// exactly 2*pi*df*Tu. Unambiguous over +-500 Hz, which is half a carrier
	// spacing - and the fractional part is all that matters (see the header).
	float re = 0.0f;
	float im = 0.0f;
	for (int s = 0; s < symbols; s++) {
		const dsp::Cf* g = x + start + s * kSymbolSamples;
		const dsp::Cf* t = g + kUsefulSamples;
		for (int k = 0; k < kGuardSamples; k++) {
			re += t[k].re * g[k].re + t[k].im * g[k].im;
			im += t[k].re * g[k].im - t[k].im * g[k].re;
		}
	}
	float phase = dsp::FastAtan2(im, re);
	return -phase / (kTwoPi * (float)kUsefulSamples / (float)kSampleRate);
}

void
DabProbe::_Analyze(const dsp::Cf* x, size_t count)
{
	fResult.analyses++;

	// 1. Null symbol: the deepest kNullSamples-long dip in mean power.
	fPowerSum.assign(count + 1, 0.0);
	for (size_t i = 0; i < count; i++) {
		fPowerSum[i + 1] = fPowerSum[i]
			+ (double)x[i].re * x[i].re + (double)x[i].im * x[i].im;
	}
	double meanPower = fPowerSum[count] / (double)count;
	if (meanPower <= 0.0) {
		fResult.locked = false;
		return;
	}

	// Search a whole frame period, so the null is found wherever it happens to
	// fall, while still leaving room for the symbols the analysis reads after
	// it. kWindowSamples is sized for exactly this - see the header.
	size_t needed = (size_t)kNullSamples
		+ (size_t)(kMeasureSymbols + 1) * kSymbolSamples;
	if (count <= needed) {
		fResult.locked = false;
		return;
	}
	size_t searchEnd = count - needed;
	if (searchEnd > (size_t)kFrameSamples)
		searchEnd = (size_t)kFrameSamples;

	double bestSum = -1.0;
	size_t nullPos = 0;
	for (size_t i = 0; i < searchEnd; i++) {
		double sum = fPowerSum[i + kNullSamples] - fPowerSum[i];
		if (bestSum < 0.0 || sum < bestSum) {
			bestSum = sum;
			nullPos = i;
		}
	}
	fResult.nullDepth = (float)(bestSum / (double)kNullSamples / meanPower);

	// A DAB null drops to a few percent of mean power. Anything above a third
	// is not a null, it is just a quiet moment.
	if (fResult.nullDepth > 0.35f) {
		fResult.locked = false;
		fResult.merDb = 0.0f;
		fResult.symbolsUsed = 0;
		return;
	}

	// 2. Symbol timing from the guard interval.
	int firstSymbol = (int)nullPos + kNullSamples;
	int shift = _Timing(x, count, firstSymbol, 320);
	fResult.timingShift = shift;
	int start = firstSymbol + shift;
	if (start < 0) {
		fResult.locked = false;
		return;
	}

	// 3. Fractional carrier offset, over enough symbols to average the noise
	// down. No coarse correction - see the header.
	int available = (int)count - start - kUsefulSamples - kGuardSamples;
	int symbols = available / kSymbolSamples;
	if (symbols > 40)
		symbols = 40;
	if (symbols < 4) {
		fResult.locked = false;
		return;
	}
	fResult.fineOffsetHz = _FineFrequency(x, start, symbols);

	// 4. FFT each symbol with that offset removed, then differentially
	// demodulate against the previous symbol and measure how far the result
	// sits from the four ideal pi/4-DQPSK points.
	int use = symbols < kMeasureSymbols ? symbols : kMeasureSymbols;

	dsp::Nco nco;
	nco.SetFrequency(-fResult.fineOffsetHz, (float)kSampleRate);

	const int kCarriers = 2 * kCarriersPerSide;
	std::vector<dsp::Cf> prev(kCarriers);
	std::vector<dsp::Cf> cur(kCarriers);
	double errSum = 0.0;
	size_t errCount = 0;
	int used = 0;

	for (int s = 0; s < use; s++) {
		int o = start + s * kSymbolSamples + kGuardSamples;
		if ((size_t)(o + kUsefulSamples) > count)
			break;

		// Phase the oscillator to this symbol's absolute position rather than
		// restarting it. Restarting looks harmless and is not: the carrier
		// offset's phase advance across the gap between symbols is exactly
		// what differential demodulation would otherwise cancel, so throwing
		// it away rotates every DQPSK decision by 2*pi*df*Ts - 112 degrees at
		// a 250 Hz offset. The constellation still measures 11 dB MER,
		// because that only asks how far each point is from the *nearest*
		// ideal one, and not a single FIB passed CRC until this was fixed.
		nco.SetPhaseForSample((unsigned int)o);
		memcpy(&fSymbol[0], x + o, kUsefulSamples * sizeof(dsp::Cf));
		nco.Mix(&fSymbol[0], kUsefulSamples, &fSymbol[0]);

		dsp::Fft(&fSymbol[0], kUsefulSamples);

		// In frequency-deinterleaved order: logical bit k comes from the
		// carrier the standard's permutation puts there. Carrier 0 is never
		// used by DAB, which is convenient - that is exactly where the
		// dongle's DC offset sits.
		for (int k = 0; k < kCarriers; k++)
			cur[k] = fSymbol[fMapper[k]];

		if (s > 0) {
			// Symbols 1, 2 and 3 are the FIC. Their soft bits are laid out as
			// all the real parts of a symbol followed by all the imaginary
			// parts - not interleaved per carrier, which decodes nothing.
			float* ficRe = NULL;
			if (s >= 1 && s <= 3)
				ficRe = &fFicSoft[(s - 1) * 2 * kCarriers];

			for (int k = 0; k < kCarriers; k++) {
				// cur * conj(prev), normalised to the unit circle.
				float re = cur[k].re * prev[k].re + cur[k].im * prev[k].im;
				float im = cur[k].im * prev[k].re - cur[k].re * prev[k].im;
				float mag = sqrtf(re * re + im * im);
				if (mag < 1e-12f)
					continue;
				float ang = dsp::FastAtan2(im, re);
				// Nearest of +-45, +-135 degrees.
				float quarter = 1.57079633f;
				float offset = 0.78539816f;
				float n = (ang - offset) / quarter;
				n = n >= 0.0f ? floorf(n + 0.5f) : ceilf(n - 0.5f);
				float ideal = n * quarter + offset;
				float err = ang - ideal;
				while (err > 3.14159274f)
					err -= kTwoPi;
				while (err < -3.14159274f)
					err += kTwoPi;
				// Chord length for a unit-radius constellation.
				float evm = 2.0f * sinf(err * 0.5f);
				errSum += (double)evm * evm;
				errCount++;

				if (ficRe != NULL) {
					ficRe[k] = re / mag;
					ficRe[kCarriers + k] = im / mag;
				}
			}
			used++;
		}
		cur.swap(prev);
	}

	fResult.symbolsUsed = used;
	if (errCount == 0) {
		fResult.locked = false;
		fResult.merDb = 0.0f;
		return;
	}

	double evmRms = sqrt(errSum / (double)errCount);
	fResult.merDb = (float)(-20.0 * log10(evmRms + 1e-12));

	// Try the FIC whenever there is a null and enough symbols, without
	// gating on MER first. A FIB CRC passing is a far stronger statement
	// than any MER threshold - it cannot happen by accident - and gating on
	// MER > 7 dB meant that a multiplex measuring 6.1 dB was refused before
	// the one test that could have settled it. Decoding costs four Viterbi
	// runs a second, which is nothing.
	if (used >= 3) {
		int fibs = fFic.DecodeFrame(&fFicSoft[0]);
		fResult.fibsOk = (uint32)fFic.FibsOk();
		fResult.fibsTried = (uint32)fFic.FibsTried();
		fResult.fibsThisFrame = fibs;
	}

	// Locked means "this is demodulating": FIBs passing CRC if the FIC came
	// through, and the constellation measurement otherwise.
	fResult.locked = fResult.fibsThisFrame >= 4
		|| (fResult.merDb > 7.0f && used >= 8);
	if (fResult.locked)
		fResult.locks++;
}
