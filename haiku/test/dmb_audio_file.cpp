/* Decode ER-BSAC audio from a recovered T-DMB transport stream. */
#include <cstdio>
#include <vector>

#include "DmbAudio.h"

int
main(int argc, char** argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <input.ts>\n", argv[0]);
		return 2;
	}
	FILE* f = fopen(argv[1], "rb");
	if (f == NULL)
		return 2;
	DmbAudio decoder;
	std::vector<unsigned char> packets(188 * 64);
	std::vector<float> audio;
	while (true) {
		size_t bytes = fread(&packets[0], 1, packets.size(), f);
		bytes -= bytes % 188;
		if (bytes == 0)
			break;
		decoder.Feed(&packets[0], bytes, audio);
	}
	fclose(f);
	const DmbAudio::stats& s = decoder.Stats();
	printf("PID 0x%04x, %d Hz, %d channels, decoded %llu/%llu AUs, "
		"%lu mono samples\n", s.audioPid, s.sampleRate, s.channels,
		(unsigned long long)s.decodedUnits,
		(unsigned long long)s.accessUnits, (unsigned long)audio.size());
	return s.decodedUnits > 0 && !audio.empty() ? 0 : 1;
}
