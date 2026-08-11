/*
 * Measures how far each airband channel actually stands above the noise floor.
 *
 * The squelch threshold in AirMonitor is a number of dB over the measured
 * floor, and picking it by intuition was wrong once already. This captures a
 * couple of seconds at each window centre and prints the real figures, so the
 * threshold can be set from data.
 *
 *   air_probe <city-index> [seconds-per-window] [gain-tenths]
 */
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <vector>
#include <unistd.h>

#include <rtl-sdr.h>

#include "SupportDefs.h"
#include "AirBands.h"
#include "AirMonitor.h"
#include "Dsp.h"

int
main(int argc, char** argv)
{
	int cityIndex = argc > 1 ? atoi(argv[1]) : 0;
	int seconds = argc > 2 ? atoi(argv[2]) : 2;
	int gain = argc > 3 ? atoi(argv[3]) : 328;

	const air_city& city = AirCityAt(cityIndex);
	printf("city %s, %d channels, gain %.1f dB\n", city.name, city.count,
		gain / 10.0);

	AirMonitor monitor;
	std::vector<AirMonitor::channel> chans;
	for (int i = 0; i < city.count; i++) {
		AirMonitor::channel c;
		c.label = city.channels[i].label;
		c.hz = city.channels[i].hz;
		chans.push_back(c);
	}
	monitor.SetChannels(chans);
	printf("grouped into %d tuner windows\n\n", monitor.WindowCount());

	rtlsdr_dev_t* dev = NULL;
	// Avoid a temporary count-only libusb context immediately before the live
	// one; Darwin can still be tearing its hotplug loop down during open.
	if (rtlsdr_open(&dev, 0) < 0 || dev == NULL) {
		fprintf(stderr, "no dongle\n");
		return 1;
	}
	rtlsdr_set_sample_rate(dev, AirMonitor::kTunerRate);
	rtlsdr_set_agc_mode(dev, 0);
	rtlsdr_set_tuner_gain_mode(dev, 1);
	rtlsdr_set_tuner_gain(dev, gain);

	const int kFft = 4096;
	std::vector<dsp::Cf> buf(kFft);
	std::vector<float> power(kFft);
	std::vector<uint8> raw((size_t)kFft * 2);

	for (int w = 0; w < monitor.WindowCount(); w++) {
		// Walk the windows by asking the monitor to advance; it exposes the
		// centre it wants, which is what the app tunes to.
		uint64 centre = 0;
		{
			// Rebuild to reach window w: the monitor advances on its own, so
			// recompute the grouping here rather than poking at its state.
			std::vector<uint64> hz;
			for (int i = 0; i < city.count; i++)
				hz.push_back(city.channels[i].hz);
			std::sort(hz.begin(), hz.end());
			const double span = (double)AirMonitor::kTunerRate * 0.8;
			int win = 0;
			size_t i = 0;
			while (i < hz.size()) {
				uint64 low = hz[i];
				size_t j = i;
				while (j + 1 < hz.size() && (double)(hz[j + 1] - low) <= span)
					j++;
				if (win == w) {
					centre = (low + hz[j]) / 2;
					break;
				}
				win++;
				i = j + 1;
			}
		}
		if (centre == 0)
			continue;

		rtlsdr_set_center_freq(dev, (uint32)centre);
		rtlsdr_reset_buffer(dev);
		usleep(120000);

		std::fill(power.begin(), power.end(), 0.0f);
		int segments = 0;
		int want = seconds * (int)AirMonitor::kTunerRate / kFft;
		float peakSeen[64];
		for (int i = 0; i < 64; i++)
			peakSeen[i] = -200.0f;

		for (int seg = 0; seg < want; seg++) {
			int got = 0;
			if (rtlsdr_read_sync(dev, &raw[0], (int)raw.size(), &got) < 0)
				break;
			if (got < (int)raw.size())
				continue;
			for (int i = 0; i < kFft; i++) {
				float win2 = 0.5f - 0.5f * cosf(2.0f * 3.14159265f * i
					/ (kFft - 1));
				buf[i].re = ((float)raw[2 * i] - 127.4f) / 128.0f * win2;
				buf[i].im = ((float)raw[2 * i + 1] - 127.4f) / 128.0f * win2;
			}
			dsp::Fft(&buf[0], kFft);
			std::vector<float> p(kFft);
			for (int i = 0; i < kFft; i++)
				p[i] = buf[i].re * buf[i].re + buf[i].im * buf[i].im;

			std::vector<float> sorted(p);
			std::sort(sorted.begin(), sorted.end());
			float floorDb = 10.0f * log10f(sorted[kFft / 2] + 1e-20f);

			// Per-segment peak, so a short transmission is not averaged away.
			double hzPerBin = (double)AirMonitor::kTunerRate / kFft;
			for (int c = 0; c < city.count && c < 64; c++) {
				double off = (double)city.channels[c].hz - (double)centre;
				int cb = (int)llround(off / hzPerBin);
				int half = (int)(4000.0 / hzPerBin);
				if (cb - half < -kFft / 2 || cb + half > kFft / 2)
					continue;
				double sum = 0.0;
				int n = 0;
				for (int k = cb - half; k <= cb + half; k++) {
					int bin = k < 0 ? k + kFft : k;
					sum += p[bin];
					n++;
				}
				if (n == 0)
					continue;
				float db = 10.0f * log10f((float)(sum / n) + 1e-20f) - floorDb;
				if (db > peakSeen[c])
					peakSeen[c] = db;
			}
			segments++;
		}

		printf("window %d centred %.3f MHz, %d segments over %d s:\n",
			w, centre / 1e6, segments, seconds);
		for (int c = 0; c < city.count && c < 64; c++) {
			if (peakSeen[c] <= -200.0f)
				continue;
			printf("   %-22s %8.3f MHz   peak %+6.1f dB over floor\n",
				city.channels[c].label, city.channels[c].hz / 1e6,
				peakSeen[c]);
		}
		printf("\n");
	}

	rtlsdr_close(dev);
	return 0;
}
