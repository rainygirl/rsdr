/*
 * Checks a recovered T-DMB transport stream end to end, offline.
 *
 * Reports what the object descriptor stream says the service contains, how
 * many BSAC access units decoded to PCM, and how many H.264 access units came
 * out of the video elementary stream - which is everything the player needs
 * before any of it is wired to a window.
 *
 *   ts_probe <file.ts> [mono-pcm.f32]
 */
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

#include "SupportDefs.h"
#include "DmbAudio.h"

int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <file.ts>\n", argv[0]);
		return 1;
	}
	FILE* f = fopen(argv[1], "rb");
	if (f == NULL) {
		fprintf(stderr, "cannot open %s\n", argv[1]);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::vector<uint8> ts((size_t)size);
	if (fread(&ts[0], 1, (size_t)size, f) != (size_t)size) {
		fprintf(stderr, "short read\n");
		return 1;
	}
	fclose(f);
	printf("%s: %ld bytes, %ld packets\n", argv[1], size, size / 188);

	DmbAudio demux;
	std::vector<float> pcm;
	demux.Feed(&ts[0], ts.size(), pcm);

	const DmbAudio::stats& st = demux.Stats();
	printf("audio: PID 0x%04X, %d Hz, %d channels, %llu AUs, %llu decoded, "
		"%llu errors\n", st.audioPid, st.sampleRate, st.channels,
		(unsigned long long)st.accessUnits,
		(unsigned long long)st.decodedUnits,
		(unsigned long long)st.errors);
	printf("       %lu mono frames of PCM\n", (unsigned long)pcm.size());
	printf("       max AU RMS %.3f, diff RMS %.3f, boundary %.3f, "
		"loud %llu, concealed %llu\n",
		st.maxUnitRms, st.maxDifferenceRms, st.maxBoundaryJump,
		(unsigned long long)st.loudUnits,
		(unsigned long long)st.concealedUnits);
	printf("       SL headers: 5=%llu 9=%llu other=%llu\n",
		(unsigned long long)st.slHeader5,
		(unsigned long long)st.slHeader9,
		(unsigned long long)st.slHeaderOther);
	std::vector<DmbAudio::audio_anchor> anchors;
	demux.TakeAudioAnchors(anchors);
	if (!anchors.empty()) {
		printf("       %lu PTS anchors, %.3f -> %.3f s\n",
			(unsigned long)anchors.size(), anchors.front().pts90k / 90000.0,
			anchors.back().pts90k / 90000.0);
	}
	if (argc >= 3 && !pcm.empty()) {
		FILE* out = fopen(argv[2], "wb");
		if (out != NULL) {
			fwrite(&pcm[0], sizeof(float), pcm.size(), out);
			fclose(out);
			printf("       wrote %s\n", argv[2]);
		}
	}

	printf("ES map:");
	for (std::map<int, int>::const_iterator i = demux.EsPidMap().begin();
			i != demux.EsPidMap().end(); ++i) {
		printf("  ES %d -> PID 0x%04X", i->first, i->second);
	}
	printf("\n       audio ES %d, video ES %d\n", demux.AudioEsId(),
		demux.VideoEsId());
	printf("       SL audio:");
	for (size_t i = 0; i < demux.AudioSlConfig().size(); i++)
		printf(" %02X", demux.AudioSlConfig()[i]);
	printf("  video:");
	for (size_t i = 0; i < demux.VideoSlConfig().size(); i++)
		printf(" %02X", demux.VideoSlConfig()[i]);
	printf("\n");

	const std::vector<uint8>& cfg = demux.VideoConfig();
	printf("video: PID 0x%04X, avcC %lu bytes", demux.VideoPid(),
		(unsigned long)cfg.size());
	if (cfg.size() >= 4) {
		printf("  (profile 0x%02X level %.1f, NAL length %d)",
			cfg[1], cfg[3] / 10.0, (cfg[4] & 3) + 1);
	}
	printf("\n");

	DmbAudio::video_unit au;
	int units = 0;
	int timedUnits = 0;
	uint64 firstVideoPts = 0, lastVideoPts = 0;
	size_t bytes = 0;
	while (demux.TakeVideoUnit(au)) {
		units++;
		bytes += au.data.size();
		if (au.hasPts) {
			if (timedUnits == 0)
				firstVideoPts = au.pts90k;
			lastVideoPts = au.pts90k;
			timedUnits++;
		}
	}
	printf("       %d access units, %lu bytes, %d with PTS\n", units,
		(unsigned long)bytes, timedUnits);
	if (timedUnits > 0)
		printf("       video PTS %.3f -> %.3f s\n",
			firstVideoPts / 90000.0, lastVideoPts / 90000.0);
	return (st.decodedUnits > 0 || units > 0) ? 0 : 1;
}
