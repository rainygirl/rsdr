#include <cstdio>
#include <cstdlib>
#include <vector>

#include "SdrDevice.h"
#include "DabSync.h"
#include "DabFic.h"
#include "DabMsc.h"

// Headless Haiku smoke test for the real mYTN path.  It uses the same
// synchronous SdrDevice and shared DabSync/DabFic code as the GUI, but does
// not require a BWindow or a manually selected station.
int main(int argc, char** argv)
{
	int seconds = argc > 1 ? atoi(argv[1]) : 30;
	int gain = argc > 2 ? atoi(argv[2]) : 360;
	int ppm = argc > 3 ? atoi(argv[3]) : 0;
	SdrDevice dev;
	if (dev.Open(0) != B_OK) { fprintf(stderr, "no dongle\n"); return 1; }
	dev.RequestSampleRate(2048000);
	dev.RequestFrequency(183008000); // T-DMB 8B, YTN DMB / mYTN
	dev.RequestGain(gain);
	dev.RequestPpmCorrection(ppm);
	if (dev.Start() != B_OK) { fprintf(stderr, "start failed\n"); return 1; }
	DabSync sync;
	DabFic fic;
	DabMsc msc;
	bool mscConfigured = false;
	std::vector<dsp::Cf> iq;
	std::vector<float> soft((size_t)(DabSync::kSymbolsPerFrame - 1)
		* DabSync::kSoftPerSymbol);
	int pos[DabSync::kSymbolsPerFrame];
	bigtime_t stop = system_time() + (bigtime_t)seconds * 1000000;
	uint64 frames = 0;
	while (system_time() < stop) {
		SdrDevice::Block block;
		if (dev.NextBlock(block, 500000) != B_OK) continue;
		size_t count = block.data.size() / 2;
		iq.resize(count);
		for (size_t i = 0; i < count; i++) {
			iq[i].re = ((float)block.data[2*i] - 127.4f) / 128.0f;
			iq[i].im = ((float)block.data[2*i+1] - 127.4f) / 128.0f;
		}
		sync.SetBuffer(&iq[0], count);
		size_t from = 0;
		int expected = -1;
		while (true) {
			float fine = 0, depth = 1;
			int start = expected >= 0 ? sync.NextFrame(expected, &fine)
				: sync.FindFrame(from, &fine, &depth);
			if (start < 0) break;
			from = (size_t)start + (DabSync::kSymbolsPerFrame - 2)
				* DabSync::kSymbolSamples;
			if (!sync.TrackFrame(start, fine, pos)) { expected = -1; continue; }
			expected = pos[DabSync::kSymbolsPerFrame - 1]
			+ DabSync::kSymbolSamples + DabSync::kNullSamples;
			sync.SoftBits(pos, fine, &soft[0]);
			// Symbols 1-3 occupy the first three soft-bit chunks.
			fic.DecodeFrame(&soft[0]);
			if (!mscConfigured) {
				for (size_t i = 0; i < fic.Subchannels().size(); i++) {
					const DabFic::subchannel& sc = fic.Subchannels()[i];
					if (sc.id != 1 || sc.shortForm || sc.sizeCu <= 0)
						continue;
					DabMsc::config c;
					c.startCu = sc.startCu;
					c.sizeCu = sc.sizeCu;
					c.protectionLevel = sc.protectionLevel;
					c.eepOptionB = sc.eepOptionB;
					mscConfigured = msc.Configure(c);
					break;
				}
			}
			if (mscConfigured) {
				const float* mscBits = &soft[(size_t)3 * DabSync::kSoftPerSymbol];
				for (int cif = 0; cif < 4; cif++)
					msc.PushCif(mscBits + (size_t)cif * DabMsc::kCifBits);
			}
			frames++;
		}
		if ((frames % 10) == 0) {
			printf("frames %llu FIB %llu/%llu ensemble %s services %lu RS %llu/%llu drops %llu\n",
				(unsigned long long)frames,
				(unsigned long long)fic.FibsOk(),
				(unsigned long long)fic.FibsTried(), fic.EnsembleLabel().c_str(),
				(unsigned long)fic.Services().size(),
				(unsigned long long)msc.Stats().rsOk,
				(unsigned long long)msc.Stats().blocksTried,
				(unsigned long long)dev.DroppedBlocks());
			fflush(stdout);
		}
	}
	dev.Stop();
	printf("DONE frames %llu FIB %llu/%llu ensemble %s services %lu RS %llu/%llu drops %llu\n",
		(unsigned long long)frames, (unsigned long long)fic.FibsOk(),
		(unsigned long long)fic.FibsTried(), fic.EnsembleLabel().c_str(),
		(unsigned long)fic.Services().size(),
		(unsigned long long)msc.Stats().rsOk,
		(unsigned long long)msc.Stats().blocksTried,
		(unsigned long long)dev.DroppedBlocks());
	return 0;
}
