/*
 * Offline T-DMB decode of a raw I/Q capture: MSC subchannel in, MPEG-2
 * transport stream out.
 *
 * This is the regression test for the whole DAB chain, and it has a hard
 * oracle rather than a plausible-looking one: RS(204,188) blocks that decode
 * with *zero* corrections. A single such block cannot happen unless frame
 * timing, per-symbol tracking, differential demodulation, carrier ordering,
 * time deinterleaving, EEP depuncturing, Viterbi, energy dispersal and the
 * Forney byte deinterleaver are all simultaneously right.
 *
 *   msc_file <capture.iq> [subchannel-id] [out.ts]
 *
 * The capture must be 8-bit unsigned I/Q at 2.048 MS/s, as iq_burst writes it.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "DabFic.h"
#include "DabMsc.h"
#include "DabSync.h"
#include "Dsp.h"

static const char*
AudioObjectName(int aot)
{
	switch (aot) {
		case 1: return "AAC Main";
		case 2: return "AAC LC";
		case 3: return "AAC SSR";
		case 4: return "AAC LTP";
		case 5: return "HE-AAC (SBR)";
		case 6: return "AAC Scalable";
		case 17: return "ER AAC LC";
		case 19: return "ER AAC LTP";
		case 20: return "ER AAC Scalable";
		case 22: return "ER BSAC";
		case 23: return "ER AAC LD";
		case 29: return "HE-AAC v2 (PS)";
		default: return "?";
	}
}

// What the elementary streams are, read out of the recovered TS rather than
// assumed. The MPEG-4 DecoderConfigDescriptor is found by pattern rather than
// by walking the whole IOD/OD tree: tag 0x04, a length, then an
// objectTypeIndication, and 13 bytes of fixed fields before the
// DecoderSpecificInfo. That is enough to be sure which codec is in use, and it
// does not depend on getting every optional field of every enclosing
// descriptor right.
static void
InspectTs(const std::vector<unsigned char>& ts)
{
	if (ts.empty()) {
		printf("no transport stream to inspect\n");
		return;
	}
	size_t n = ts.size() / 188;
	bool sawAudio = false;
	bool sawVideo = false;

	for (size_t i = 0; i < n; i++) {
		const unsigned char* p = &ts[i * 188];
		if (p[0] != 0x47)
			continue;
		int afc = (p[3] >> 4) & 3;
		int body = 4;
		if (afc == 2 || afc == 3)
			body += 1 + p[4];
		if (body >= 188)
			continue;
		const unsigned char* b = p + body;
		int len = 188 - body;

		for (int k = 0; k + 20 < len; k++) {
			if (b[k] != 0x04)
				continue;
			int size = b[k + 1];
			if (size < 15 || k + 2 + size > len)
				continue;
			const unsigned char* d = b + k + 2;
			int oti = d[0];
			int streamType = (d[1] >> 2) & 0x3F;
			if (d[13] != 0x05)
				continue;
			int dsiLen = d[14];
			if (dsiLen < 1 || 15 + dsiLen > size)
				continue;
			const unsigned char* dsi = d + 15;

			if (oti == 0x40 && streamType == 5) {
				int aot = (dsi[0] >> 3) & 0x1F;
				int fi = dsiLen >= 2
					? (((dsi[0] & 0x07) << 1) | (dsi[1] >> 7)) : -1;
				int ch = dsiLen >= 2 ? ((dsi[1] >> 3) & 0x0F) : -1;
				static const int kRates[13] = {96000, 88200, 64000, 48000,
					44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000,
					7350};
				printf("audio ES: objectTypeIndication 0x%02X, "
					"audioObjectType %d = %s", oti, aot,
					AudioObjectName(aot));
				if (fi >= 0 && fi < 13)
					printf(", %d Hz", kRates[fi]);
				if (ch >= 0)
					printf(", %d channel(s)", ch);
				printf("\n  AudioSpecificConfig:");
				for (int q = 0; q < dsiLen; q++)
					printf(" %02X", dsi[q]);
				printf("\n");
				sawAudio = true;
			} else if (oti == 0x21 && !sawVideo) {
				printf("video ES: objectTypeIndication 0x21 = H.264");
				if (dsiLen >= 4) {
					printf(", profile 0x%02X level %d (%.1f)", dsi[1], dsi[3],
						dsi[3] / 10.0);
				}
				printf("\n");
				sawVideo = true;
			}
			if (sawAudio)
				return;
		}
	}
	if (!sawAudio)
		printf("no audio DecoderConfigDescriptor seen in %lu packets\n",
			(unsigned long)n);
}

int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <capture.iq> [subchannel-id] [out.ts]\n",
			argv[0]);
		return 1;
	}
	int wantSub = argc > 2 ? atoi(argv[2]) : -1;
	const char* outPath = argc > 3 ? argv[3] : NULL;

	FILE* f = fopen(argv[1], "rb");
	if (f == NULL) {
		fprintf(stderr, "cannot open %s\n", argv[1]);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	long bytes = ftell(f);
	fseek(f, 0, SEEK_SET);
	size_t count = (size_t)bytes / 2;
	std::vector<unsigned char> raw((size_t)bytes);
	if (fread(&raw[0], 1, (size_t)bytes, f) != (size_t)bytes) {
		fprintf(stderr, "short read\n");
		return 1;
	}
	fclose(f);

	std::vector<dsp::Cf> x(count);
	for (size_t i = 0; i < count; i++) {
		x[i].re = ((float)raw[2 * i] - 127.4f) * (1.0f / 128.0f);
		x[i].im = ((float)raw[2 * i + 1] - 127.4f) * (1.0f / 128.0f);
	}
	printf("%s: %lu samples (%.3f s)\n", argv[1], (unsigned long)count,
		(double)count / 2048000.0);

	DabSync sync;
	sync.SetBuffer(&x[0], count);
	DabFic fic;
	DabMsc msc;

	std::vector<float> soft((size_t)(DabSync::kSymbolsPerFrame - 1)
		* DabSync::kSoftPerSymbol);
	std::vector<int> pos(DabSync::kSymbolsPerFrame);

	size_t searchFrom = 0;
	int frames = 0;
	int tracked = 0;
	int skipped = 0;
	int ficFull = 0;
	int cifs = 0;
	int configuredSub = -1;
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
		frames++;
		// Next search begins after this frame's symbols, wherever they end up.
		searchFrom = (size_t)start
			+ (size_t)(DabSync::kSymbolsPerFrame - 2) * DabSync::kSymbolSamples;

		if (!sync.TrackFrame(start, fineHz, &pos[0])) {
			expectedNext = -1;
			// A frame that cannot be tracked still occupied its 96 ms on air,
			// so its four CIFs have to be accounted for. Skipping them shifts
			// the 16-CIF time interleaver by four and every output logical
			// frame after that point is assembled from the wrong CIFs -
			// measured as RS 2 of 72 blocks against 47 of 60 with the phase
			// intact, on captures whose FIC was 180/180 either way. Feed
			// erasures instead, which the EEP and RS layers can absorb.
			if (msc.Configured()) {
				static std::vector<float> erasure(DabMsc::kCifBits, 0.0f);
				for (int c = 0; c < 4; c++)
					msc.PushCif(&erasure[0]);
			}
			skipped++;
			continue;
		}
		expectedNext = pos[DabSync::kSymbolsPerFrame - 1]
			+ DabSync::kSymbolSamples + DabSync::kNullSamples;
		tracked++;
		sync.SoftBits(&pos[0], fineHz, &soft[0]);

		// Symbols 1..3 are the FIC.
		int fibs = fic.DecodeFrame(&soft[0]);
		if (fibs >= 12)
			ficFull++;

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
				if (msc.Configure(c)) {
					configuredSub = subs[i].id;
					printf("subchannel %d: start %d, %d CU, EEP-%c level %d "
						"-> %d kbps useful\n", subs[i].id, c.startCu, c.sizeCu,
						c.eepOptionB ? 'B' : 'A', c.protectionLevel,
						msc.Stats().bitrateKbps);
					break;
				}
			}
		}

		// Symbols 4..75 are the MSC: 72 symbols, four CIFs.
		if (msc.Configured()) {
			const float* mscSoft = &soft[(size_t)3 * DabSync::kSoftPerSymbol];
			for (int c = 0; c < 4; c++) {
				msc.PushCif(mscSoft + (size_t)c * DabMsc::kCifBits);
				cifs++;
			}
		}
	}

	printf("frames found %d, tracked %d (%d erased), FIC 12/12 on %d\n",
		frames, tracked, skipped, ficFull);
	printf("FIC: %llu of %llu FIBs, ensemble %s\n",
		(unsigned long long)fic.FibsOk(), (unsigned long long)fic.FibsTried(),
		fic.EnsembleLabel().empty() ? "-" : fic.EnsembleLabel().c_str());
	const std::vector<DabFic::service>& services = fic.Services();
	for (size_t i = 0; i < services.size(); i++) {
		printf("service 0x%08lx %s: %s, type %d, subchannel %d, SCId %d%s\n",
			(unsigned long)services[i].id,
			services[i].label.empty() ? "(unlabelled)" : services[i].label.c_str(),
			services[i].isAudio ? "audio" : "data", services[i].type,
			services[i].subchannel, services[i].scid,
			services[i].isPrimary ? ", primary" : "");
	}

	if (configuredSub < 0) {
		printf("no usable subchannel found\n");
		return 1;
	}
	const DabMsc::stats& st = msc.Stats();
	printf("MSC subchannel %d: %llu CIFs in, %llu logical frames, "
		"RS %llu of %llu blocks", configuredSub,
		(unsigned long long)st.cifsIn, (unsigned long long)st.framesOut,
		(unsigned long long)st.rsOk, (unsigned long long)st.blocksTried);
	if (st.blocksTried > 0) {
		printf(" (%.0f%%), %llu bytes corrected",
			100.0 * (double)st.rsOk / (double)st.blocksTried,
			(unsigned long long)st.rsCorrected);
	}
	printf("\nRS block sync: %s\n", st.synced ? "found" : "NOT found");

	const std::vector<unsigned char>& ts = msc.Packets();
	printf("transport stream: %lu bytes, %lu packets\n",
		(unsigned long)ts.size(), (unsigned long)(ts.size() / 188));
	InspectTs(ts);
	if (outPath != NULL && !ts.empty()) {
		FILE* o = fopen(outPath, "wb");
		if (o != NULL) {
			fwrite(&ts[0], 1, ts.size(), o);
			fclose(o);
			printf("wrote %s\n", outPath);
		}
	}
	return st.rsOk > 0 ? 0 : 1;
}
