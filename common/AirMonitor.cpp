#include "AirMonitor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// Usable fraction of the tuner window. The edges are the decimation filter's
// transition region and the dongle's own DC spur sits dead centre, so channels
// are only placed where both are avoided.
const double kUsableFraction = 0.8;
const int kFftSize = 4096;
// Airband AM voice is about 6 kHz wide in a 25 kHz channel.
const double kChannelHalfWidthHz = 4000.0;

const bigtime_t kHoldAfterSignal = 1500000;		// stay 1.5 s after it ends
const bigtime_t kWindowDwell = 700000;			// then move on after this

}

AirMonitor::AirMonitor()
	:
	fWindow(0),
	fRetune(true),
	fActive(-1),
	fThresholdDb(3.0f),
	fActiveAt(0),
	fWindowAt(0),
	fConfiguredFor(-1),
	fSampleIndex(0)
{
	fFftBuf.resize(kFftSize);
	fPower.resize(kFftSize);
}

void
AirMonitor::Reset()
{
	fWindow = 0;
	fRetune = true;
	fActive = -1;
	fActiveAt = 0;
	fWindowAt = 0;
	fConfiguredFor = -1;
	fSampleIndex = 0;
	for (size_t i = 0; i < fChannels.size(); i++) {
		fChannels[i].levelDb = -140.0f;
		fChannels[i].open = false;
	}
}

void
AirMonitor::SetChannels(const std::vector<channel>& channels)
{
	fChannels = channels;
	fWindows.clear();

	// Sort by frequency, then take greedy runs that fit in one usable window.
	std::vector<int> order;
	for (size_t i = 0; i < fChannels.size(); i++)
		order.push_back((int)i);
	for (size_t a = 0; a + 1 < order.size(); a++) {
		for (size_t b = a + 1; b < order.size(); b++) {
			if (fChannels[order[b]].hz < fChannels[order[a]].hz)
				std::swap(order[a], order[b]);
		}
	}

	const double span = (double)kTunerRate * kUsableFraction;
	size_t i = 0;
	while (i < order.size()) {
		uint64 low = fChannels[order[i]].hz;
		size_t j = i;
		while (j + 1 < order.size()
			&& (double)(fChannels[order[j + 1]].hz - low) <= span) {
			j++;
		}
		window w;
		uint64 high = fChannels[order[j]].hz;
		w.centreHz = (low + high) / 2;
		for (size_t k = i; k <= j; k++)
			w.channels.push_back(order[k]);
		fWindows.push_back(w);
		i = j + 1;
	}
	Reset();
}

uint64
AirMonitor::TuneHz() const
{
	if (fWindows.empty())
		return 0;
	int w = fWindow;
	if (w < 0 || w >= (int)fWindows.size())
		w = 0;
	return fWindows[w].centreHz;
}

void
AirMonitor::_MeasureLevels(const dsp::Cf* x, size_t count)
{
	if (fWindows.empty() || count < (size_t)kFftSize)
		return;
	const window& w = fWindows[fWindow];

	// Average a few periodograms so a single noisy transform cannot open the
	// squelch on its own.
	std::fill(fPower.begin(), fPower.end(), 0.0f);
	int segments = 0;
	for (size_t at = 0; at + kFftSize <= count && segments < 8;
			at += (size_t)kFftSize, segments++) {
		for (int i = 0; i < kFftSize; i++) {
			// Hann, so a strong carrier does not smear into its neighbours.
			float win = 0.5f - 0.5f * cosf(2.0f * 3.14159265f * i
				/ (kFftSize - 1));
			fFftBuf[i].re = x[at + i].re * win;
			fFftBuf[i].im = x[at + i].im * win;
		}
		dsp::Fft(&fFftBuf[0], kFftSize);
		for (int i = 0; i < kFftSize; i++) {
			fPower[i] += fFftBuf[i].re * fFftBuf[i].re
				+ fFftBuf[i].im * fFftBuf[i].im;
		}
	}
	if (segments == 0)
		return;

	const double hzPerBin = (double)kTunerRate / kFftSize;
	const double norm = 1.0 / ((double)segments * kFftSize * kFftSize);

	// Noise floor from the median bin, so the threshold follows the gain and
	// the band rather than an absolute level.
	std::vector<float> sorted(fPower);
	std::sort(sorted.begin(), sorted.end());
	double floorPower = sorted[sorted.size() / 2] * norm;
	float floorDb = 10.0f * log10f((float)floorPower + 1e-20f);

	for (size_t c = 0; c < w.channels.size(); c++) {
		channel& ch = fChannels[w.channels[c]];
		double offset = (double)ch.hz - (double)fWindows[fWindow].centreHz;
		int centreBin = (int)llround(offset / hzPerBin);
		int half = (int)(kChannelHalfWidthHz / hzPerBin);
		double sum = 0.0;
		int n = 0;
		for (int k = centreBin - half; k <= centreBin + half; k++) {
			int bin = k < 0 ? k + kFftSize : k;
			if (bin < 0 || bin >= kFftSize)
				continue;
			sum += fPower[bin];
			n++;
		}
		if (n == 0)
			continue;
		double p = sum / n * norm;
		ch.levelDb = 10.0f * log10f((float)p + 1e-20f);
		ch.floorDb = floorDb;
		// A carrier has to stand clear of the measured floor, not of an
		// absolute dBFS level, so one setting keeps working as the gain and
		// the band noise change. fThresholdDb is that margin in dB.
		//
		// Measured on a quiet band with this dongle: channel readings wander
		// between +5 and +9 dB over the floor on noise alone, so anything
		// below about 10 dB is not a signal. A real AM carrier is tens of dB
		// clear. The first version demanded ~20 dB and let nothing through.
		// Once a channel is selected, require it to fall another 3 dB before
		// declaring it closed.  Without hysteresis a carrier sitting near the
		// threshold alternated open/closed from one periodogram to the next.
		float threshold = fThresholdDb
			- (w.channels[c] == fActive ? 3.0f : 0.0f);
		ch.open = (ch.levelDb - floorDb) > threshold;
	}
}

void
AirMonitor::_ConfigureFor(int channelIndex)
{
	if (channelIndex == fConfiguredFor)
		return;
	fConfiguredFor = channelIndex;
	fStages.clear();
	if (channelIndex < 0)
		return;

	double offset = (double)fChannels[channelIndex].hz
		- (double)fWindows[fWindow].centreHz;
	fNco.SetFrequency((float)-offset, (float)kTunerRate);

	// 2048000 -> 256000 -> 32000 -> 16000. The first stage only has to reach
	// the next Nyquist, which is what keeps its tap count affordable at the
	// full rate.
	std::vector<float> taps;
	fStages.resize(3);
	dsp::DesignLowpass(taps, 25, 110000.0f, (float)kTunerRate);
	fStages[0].Init(taps, 8);
	dsp::DesignLowpass(taps, 61, 14000.0f, 256000.0f);
	fStages[1].Init(taps, 8);
	dsp::DesignLowpass(taps, 61, 4000.0f, 32000.0f);
	fStages[2].Init(taps, 2);

	fDcBlock.Reset();
	fAgc.Reset();
}

size_t
AirMonitor::Process(const uint8* iq, size_t bytes, std::vector<float>& out,
	int& active)
{
	active = -1;
	if (fWindows.empty() || bytes < 2)
		return 0;

	size_t count = bytes / 2;
	if (fBufA.size() < count)
		fBufA.resize(count);
	for (size_t i = 0; i < count; i++) {
		fBufA[i].re = ((float)iq[2 * i] - 127.4f) * (1.0f / 128.0f);
		fBufA[i].im = ((float)iq[2 * i + 1] - 127.4f) * (1.0f / 128.0f);
	}

	_MeasureLevels(&fBufA[0], count);

	// Strongest open channel in this window wins; ties go to the one already
	// playing so a two-way exchange does not flap.
	const window& w = fWindows[fWindow];
	int best = -1;
	float bestDb = -1e9f;
	for (size_t c = 0; c < w.channels.size(); c++) {
		const channel& ch = fChannels[w.channels[c]];
		if (!ch.open)
			continue;
		float db = ch.levelDb + (w.channels[c] == fActive ? 3.0f : 0.0f);
		if (db > bestDb) {
			bestDb = db;
			best = w.channels[c];
		}
	}

	bigtime_t now = system_time();
	if (best >= 0) {
		fActive = best;
		fActiveAt = now;
		fWindowAt = now;
	} else if (fActive >= 0 && now - fActiveAt < kHoldAfterSignal) {
		// Keep the channel selected briefly so gaps between words do not
		// bounce the monitor to another window mid-transmission.
	} else {
		fActive = -1;
		if (fWindows.size() > 1 && now - fWindowAt > kWindowDwell) {
			fWindow = (fWindow + 1) % (int)fWindows.size();
			fWindowAt = now;
			fRetune = true;
			fConfiguredFor = -1;
		}
	}

	if (fActive < 0) {
		// Nothing selected. Silence, not noise: this is the whole point.
		fSampleIndex += count;
		return 0;
	}
	// best may be -1 here while the selected channel is inside the 1.5-second
	// hold. Continue demodulating it: returning no samples during that hold
	// drained and re-primed the audio ring, producing roughly half a second of
	// sound followed by half a second of silence on a marginal carrier.

	_ConfigureFor(fActive);
	active = fActive;

	// Shift the channel to zero, decimate, envelope-detect.
	if (fBufB.size() < count)
		fBufB.resize(count);
	fNco.SetPhaseForSample((unsigned int)fSampleIndex);
	fNco.Mix(&fBufA[0], count, &fBufB[0]);
	fSampleIndex += count;

	size_t n = count;
	dsp::Cf* src = &fBufB[0];
	for (size_t s = 0; s < fStages.size(); s++)
		n = fStages[s].Process(src, n, src);

	if (fDemod.size() < n)
		fDemod.resize(n);
	for (size_t i = 0; i < n; i++)
		fDemod[i] = sqrtf(src[i].re * src[i].re + src[i].im * src[i].im);

	fDcBlock.Process(&fDemod[0], n);
	fAgc.Process(&fDemod[0], n);
	dsp::SoftClip(&fDemod[0], n, 0.70f);

	out.insert(out.end(), fDemod.begin(), fDemod.begin() + n);
	return n;
}
