/*
 * The app's live T-DMB path, without the app.
 *
 * A capture taken with the same dongle decodes perfectly offline while the
 * window shows zero frames, so the fault is somewhere between RtlDevice and
 * DabSync rather than in the RF or the decoder. This runs exactly that path
 * and prints what each block looks like.
 *
 *   dab_live <MHz> [seconds] [gain-tenths] [ppm]
 */
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>

#include "SupportDefs.h"
#include "DabFic.h"
#include "DabSync.h"
#include "Dsp.h"
#include "RtlDevice.h"

int
main(int argc, char** argv)
{
	double mhz = argc > 1 ? atof(argv[1]) : 205.28;
	int seconds = argc > 2 ? atoi(argv[2]) : 10;
	int gain = argc > 3 ? atoi(argv[3]) : 402;
	int ppm = argc > 4 ? atoi(argv[4]) : 0;

	RtlDevice dev;
	if (dev.Open(0) != B_OK) {
		fprintf(stderr, "no dongle\n");
		return 1;
	}
	printf("%s\n", dev.Description().c_str());
	dev.RequestBlockBytes(0);
	dev.RequestSampleRate(2048000);
	dev.RequestPpm(ppm);
	dev.RequestGain(gain);
	dev.RequestFrequency((uint32)(mhz * 1e6 + 0.5));
	if (dev.Start() != B_OK) {
		fprintf(stderr, "start failed\n");
		return 1;
	}

	DabSync sync;
	DabFic fic;
	std::vector<dsp::Cf> iq;
	std::vector<float> soft((size_t)(DabSync::kSymbolsPerFrame - 1)
		* DabSync::kSoftPerSymbol);
	std::vector<int> pos(DabSync::kSymbolsPerFrame);

	bigtime_t stop = system_time() + (bigtime_t)seconds * 1000000;
	std::vector<uint8> block;
	uint32 generation = 0;
	int blocks = 0;

	while (system_time() < stop) {
		if (!dev.NextBlock(block, generation, 500000)) {
			printf("no block (rate %u, lost %d)\n", (unsigned)dev.SampleRate(),
				dev.DeviceLost() ? 1 : 0);
			continue;
		}
		blocks++;
		size_t count = block.size() / 2;
		if (iq.size() < count)
			iq.resize(count);
		double power = 0.0;
		for (size_t i = 0; i < count; i++) {
			iq[i].re = ((float)block[2 * i] - 127.4f) * (1.0f / 128.0f);
			iq[i].im = ((float)block[2 * i + 1] - 127.4f) * (1.0f / 128.0f);
			if ((i & 63) == 0) {
				power += (double)iq[i].re * iq[i].re
					+ (double)iq[i].im * iq[i].im;
			}
		}
		float rf = 10.0f * log10f((float)(power / (count / 64)) + 1e-20f);

		sync.SetBuffer(&iq[0], count);
		float fineHz = 0.0f;
		float depth = 1.0f;
		int start = sync.FindFrame(0, &fineHz, &depth);
		int tracked = 0;
		int fibs = 0;
		if (start >= 0) {
			size_t from = 0;
			int expected = -1;
			while (true) {
				float f2 = 0.0f, d2 = 1.0f;
				int st = -1;
				if (expected >= 0)
					st = sync.NextFrame(expected, &f2);
				if (st < 0)
					st = sync.FindFrame(from, &f2, &d2);
				if (st < 0)
					break;
				from = (size_t)st + (size_t)(DabSync::kSymbolsPerFrame - 2)
					* DabSync::kSymbolSamples;
				if (!sync.TrackFrame(st, f2, &pos[0])) {
					expected = -1;
					continue;
				}
				expected = pos[DabSync::kSymbolsPerFrame - 1]
					+ DabSync::kSymbolSamples + DabSync::kNullSamples;
				tracked++;
				sync.SoftBits(&pos[0], f2, &soft[0]);
				fibs += fic.DecodeFrame(&soft[0]);
			}
		}
		printf("block %2d  %lu bytes  gen %u  RF %6.1f dB  null %.3f  "
			"start %7d  tracked %2d  FIBs %3d  ensemble %s\n",
			blocks, (unsigned long)block.size(), (unsigned)generation, rf,
			depth, start, tracked, fibs,
			fic.EnsembleLabel().empty() ? "-" : fic.EnsembleLabel().c_str());
	}

	dev.Stop();
	dev.Close();
	return 0;
}
