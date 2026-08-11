// Deterministic reproduction of the live Receiver's per-6MiB-read DMB processing
// on a saved capture. A single iq_burst capture has NO inter-read USB loss, so
// this isolates the frame-restart / interleaver-priming effects from the hardware
// gap. Compare RS against msc_file (whole-buffer continuous) on the same file.
//
// modes:
//   0  restart per read, carry MSC state (loses the straddling frame)
//   1  restart per read, carry, erasure for the straddling frame
//   2  restart per read, carry, pad each read to its full CIF span
//   3  reset MSC per read (independent decode) - correct but a 15-CIF priming
//      gap per read wastes ~23% of the audio
//   4  reset per read, then re-inject the previous read's last 15 CIFs to prime
//      the interleaver (recover the boundary frames, remove the priming gap)
//   5  like 4 but prime with 11 real CIFs + 4 erasures (account for the lost
//      straddling frame between reads)

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "DabSync.h"
#include "DabFic.h"
#include "DabMsc.h"

int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <capture.iq> [subchannel-id] [mode]\n", argv[0]);
		return 1;
	}
	int wantSub = argc > 2 ? atoi(argv[2]) : 1;
	int mode = argc > 3 ? atoi(argv[3]) : 0;
	// arg 5 multiplies the 6 MiB read size: bigger reads = fewer per-read resets,
	// so the byte-sync startup cost is amortised over more frames.
	int mult = argc > 4 ? atoi(argv[4]) : 1;
	size_t chunk = (size_t)3145728 * (mult < 1 ? 1 : mult);

	FILE* f = fopen(argv[1], "rb");
	if (f == NULL) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
	fseek(f, 0, SEEK_END);
	long bytes = ftell(f);
	fseek(f, 0, SEEK_SET);
	size_t count = (size_t)bytes / 2;
	std::vector<unsigned char> rawb((size_t)bytes);
	if (fread(&rawb[0], 1, (size_t)bytes, f) != (size_t)bytes) {
		fprintf(stderr, "short read\n"); return 1;
	}
	fclose(f);

	std::vector<dsp::Cf> x(count);
	for (size_t i = 0; i < count; i++) {
		x[i].re = ((float)rawb[2 * i] - 127.4f) * (1.0f / 128.0f);
		x[i].im = ((float)rawb[2 * i + 1] - 127.4f) * (1.0f / 128.0f);
	}

	DabSync sync;
	DabFic fic;
	DabMsc msc;
	std::vector<float> soft((size_t)(DabSync::kSymbolsPerFrame - 1)
		* DabSync::kSoftPerSymbol);
	std::vector<int> pos(DabSync::kSymbolsPerFrame);
	static std::vector<float> erasure(DabMsc::kCifBits, 0.0f);

	// Rolling copy of the last CIFs pushed, for priming after a reset (modes 4/5).
	std::vector<std::vector<float> > saved;
	auto pushSave = [&](const float* c) {
		msc.PushCif(c);
		saved.push_back(std::vector<float>(c, c + DabMsc::kCifBits));
		if (saved.size() > 20)
			saved.erase(saved.begin());
	};

	int tracked = 0;
	int nchunks = 0;

	for (size_t base = 0; base < count; base += chunk) {
		size_t len = count - base < chunk ? count - base : chunk;
		sync.SetBuffer(&x[base], len);
		bool firstChunk = (nchunks == 0);
		nchunks++;
		if (mode >= 3 && msc.Configured() && !firstChunk)
			msc.Reset();
		// Prime the fresh interleaver with the previous read's tail so the
		// boundary frames are not lost to the 16-CIF fill (modes 4/5).
		if (mode >= 4 && msc.Configured() && !firstChunk && !saved.empty()) {
			int real = mode == 5 ? 11 : 15;
			int have = (int)saved.size();
			int from = have - real; if (from < 0) from = 0;
			for (int i = from; i < have; i++)
				msc.PushCif(&saved[(size_t)i][0]);
			if (mode == 5)
				for (int c = 0; c < 4; c++)
					msc.PushCif(&erasure[0]);
		}
		int cifsThisChunk = 0;

		size_t searchFrom = 0;
		int expectedNext = -1;
		while (true) {
			float fineHz = 0.0f;
			float depth = 1.0f;
			int start = -1;
			if (expectedNext >= 0)
				start = sync.NextFrame(expectedNext, &fineHz);
			if (start < 0)
				start = sync.FindFrame(searchFrom, &fineHz, &depth);
			if (start < 0)
				break;
			if ((size_t)start + (size_t)DabSync::kSymbolsPerFrame
					* DabSync::kSymbolSamples > len) {
				if (mode == 1 && msc.Configured()) {
					for (int c = 0; c < 4; c++)
						pushSave(&erasure[0]);
					cifsThisChunk += 4;
				}
				break;
			}
			searchFrom = (size_t)start + (size_t)(DabSync::kSymbolsPerFrame - 2)
				* DabSync::kSymbolSamples;
			if (!sync.TrackFrame(start, fineHz, &pos[0])) {
				expectedNext = -1;
				if (msc.Configured()) {
					for (int c = 0; c < 4; c++)
						pushSave(&erasure[0]);
					cifsThisChunk += 4;
				}
				continue;
			}
			expectedNext = pos[DabSync::kSymbolsPerFrame - 1]
				+ DabSync::kSymbolSamples + DabSync::kNullSamples;
			tracked++;
			sync.SoftBits(&pos[0], fineHz, &soft[0]);
			fic.DecodeFrame(&soft[0]);
			if (!msc.Configured()) {
				const std::vector<DabFic::subchannel>& subs = fic.Subchannels();
				for (size_t i = 0; i < subs.size(); i++) {
					if (subs[i].shortForm || subs[i].sizeCu <= 0)
						continue;
					if (wantSub >= 0 && subs[i].id != wantSub)
						continue;
					DabMsc::config c;
					c.startCu = subs[i].startCu;
					c.sizeCu = subs[i].sizeCu;
					c.protectionLevel = subs[i].protectionLevel;
					c.eepOptionB = subs[i].eepOptionB;
					if (msc.Configure(c))
						break;
				}
			}
			if (msc.Configured()) {
				const float* m = &soft[(size_t)3 * DabSync::kSoftPerSymbol];
				for (int c = 0; c < 4; c++)
					pushSave(m + (size_t)c * DabMsc::kCifBits);
				cifsThisChunk += 4;
			}
		}
		if (mode == 2 && msc.Configured()) {
			int want = 4 * (int)(len / (size_t)DabSync::kFrameSamples);
			while (cifsThisChunk < want) {
				pushSave(&erasure[0]);
				cifsThisChunk++;
			}
		}
	}

	const DabMsc::stats& st = msc.Stats();
	printf("mode %d, %d reads: tracked %d, RS %llu of %llu", mode, nchunks,
		tracked, (unsigned long long)st.rsOk, (unsigned long long)st.blocksTried);
	if (st.blocksTried > 0)
		printf(" (%.0f%%)", 100.0 * (double)st.rsOk / (double)st.blocksTried);
	printf(", %llu corrected\n", (unsigned long long)st.rsCorrected);
	return 0;
}
