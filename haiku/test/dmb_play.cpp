/*
 * Delayed T-DMB audio player over the stable SdrDevice path.
 *
 * Uses SdrDevice (the same 6 MiB synchronous reads the app uses - it stops
 * cleanly, unlike a large raw rtlsdr_read_sync which wedges the Haiku USB stack
 * when interrupted). The MSC is decoded CONTINUOUSLY across reads (no per-read
 * reset, which otherwise throws away ~60% of the audio to byte-sync startup);
 * DabMsc's self-healing RS re-sync recovers from the small between-read gaps.
 * A large AudioSink ring (20 s) holds the audio the good reads produce so brief
 * bad reads do not break playback. The trade is continuity for delay, which is
 * what we want for broadcast audio.
 */
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <vector>

#include "SdrDevice.h"
#include "AudioSink.h"
#include "DabSync.h"
#include "DabFic.h"
#include "DabMsc.h"
#include "DmbAudio.h"

static volatile bool gRunning = true;
static void OnSignal(int) { gRunning = false; }

int
main(int argc, char** argv)
{
	double mhz = argc > 1 ? atof(argv[1]) : 183.008;
	int gain = argc > 2 ? atoi(argv[2]) : 328;
	int wantSub = argc > 3 ? atoi(argv[3]) : 1;

	SdrDevice dev;
	if (dev.Open(0) != B_OK) {
		fprintf(stderr, "cannot open dongle\n");
		return 1;
	}
	dev.RequestSampleRate(2048000);
	dev.RequestFrequency((uint32)(mhz * 1e6 + 0.5));
	dev.RequestGain(gain);
	if (dev.Start() != B_OK) {
		fprintf(stderr, "start failed\n");
		return 1;
	}
	signal(SIGINT, OnSignal);
	signal(SIGTERM, OnSignal);
	fprintf(stderr, "%.4f MHz, gain %d, subch %d\n", mhz, gain, wantSub);

	AudioSink audio;
	audio.SetVolume(0.85f);
	audio.SetRingSeconds(20);

	DabSync sync;
	DabFic fic;
	DabMsc msc;   // continuous: never reset across reads
	DmbAudio demux;
	std::vector<float> soft((size_t)(DabSync::kSymbolsPerFrame - 1)
		* DabSync::kSoftPerSymbol);
	std::vector<int> pos(DabSync::kSymbolsPerFrame);
	std::vector<dsp::Cf> iq;
	std::vector<float> pcm;
	static std::vector<float> erasure(DabMsc::kCifBits, 0.0f);

	uint64 blocks = 0;
	while (gRunning) {
		SdrDevice::Block block;
		if (dev.NextBlock(block, 500000) != B_OK)
			continue;
		if (block.data.empty())
			continue;

		size_t count = block.data.size() / 2;
		iq.resize(count);
		const uint8* src = &block.data[0];
		for (size_t i = 0; i < count; i++) {
			iq[i].re = ((float)src[2 * i] - 127.4f) * (1.0f / 128.0f);
			iq[i].im = ((float)src[2 * i + 1] - 127.4f) * (1.0f / 128.0f);
		}

		// Reset MSC per read: each 6 MiB read is one clean synchronous transfer,
		// so decode it independently. Carrying MSC state across the between-read
		// gap never re-establishes RS byte sync (measured: 0 RS). This yields
		// clean audio on good reads and nothing on faded ones.
		if (msc.Configured())
			msc.Reset();
		sync.SetBuffer(&iq[0], count);
		size_t searchFrom = 0;
		int expectedNext = -1;
		pcm.clear();
		while (true) {
			float fineHz = 0.0f, depth = 1.0f;
			int start = expectedNext >= 0 ? sync.NextFrame(expectedNext, &fineHz)
				: sync.FindFrame(searchFrom, &fineHz, &depth);
			if (start < 0 && expectedNext >= 0)
				start = sync.FindFrame(searchFrom, &fineHz, &depth);
			if (start < 0)
				break;
			if ((size_t)start + (size_t)DabSync::kSymbolsPerFrame
					* DabSync::kSymbolSamples > count)
				break;
			searchFrom = (size_t)start + (size_t)(DabSync::kSymbolsPerFrame - 2)
				* DabSync::kSymbolSamples;
			if (!sync.TrackFrame(start, fineHz, &pos[0])) {
				expectedNext = -1;
				if (msc.Configured())
					for (int c = 0; c < 4; c++)
						msc.PushCif(&erasure[0]);
				continue;
			}
			expectedNext = pos[DabSync::kSymbolsPerFrame - 1]
				+ DabSync::kSymbolSamples + DabSync::kNullSamples;
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
				for (int c = 0; c < 4; c++) {
					msc.PushCif(m + (size_t)c * DabMsc::kCifBits);
					const std::vector<uint8>& tp = msc.Packets();
					if (!tp.empty()) {
						demux.Feed(&tp[0], tp.size(), pcm);
						msc.ClearPackets();
					}
				}
			}
		}

		const DmbAudio::stats& ds = demux.Stats();
		if (!pcm.empty()) {
			if (ds.sampleRate > 0)
				audio.Configure((uint32)ds.sampleRate, 1);
			audio.Write(&pcm[0], pcm.size());
		}
		if ((++blocks % 4) == 0) {
			fprintf(stderr, "block %llu: RS %llu/%llu, AU %llu, under %llu, "
				"peak %.2f\n", (unsigned long long)blocks,
				(unsigned long long)msc.Stats().rsOk,
				(unsigned long long)msc.Stats().blocksTried,
				(unsigned long long)ds.decodedUnits,
				(unsigned long long)audio.Underruns(), audio.PeakLevel());
			fflush(stderr);
		}
	}

	dev.Stop();
	return 0;
}
