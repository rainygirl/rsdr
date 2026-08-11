#include <stdio.h>
#include <stdlib.h>

#include <fstream>
#include <string>

#include "TsDescrambler.h"

static void
Usage(const char* name)
{
	fprintf(stderr, "usage:\n"
		"  %s --self-test\n"
		"  %s <input.ts> <output.ts> [--even HEX12|16] [--odd HEX12|16] [--pid HEX]\n",
		name, name);
}

int
main(int argc, char** argv)
{
	TsDescrambler descrambler;
	std::string error;
	if (argc == 2 && std::string(argv[1]) == "--self-test") {
		if (!descrambler.SelfTest(error)) {
			fprintf(stderr, "self-test failed: %s\n", error.c_str());
			return 1;
		}
		printf("DVB-CSA known-vector and decrypt round-trip: ok\n");
		return 0;
	}
	if (argc < 5) {
		Usage(argv[0]);
		return 2;
	}
	int onlyPid = -1;
	bool haveKey = false;
	for (int i = 3; i < argc; i++) {
		std::string option = argv[i];
		if (i + 1 >= argc) {
			Usage(argv[0]);
			return 2;
		}
		if (option == "--even") {
			haveKey = descrambler.SetEvenKey(argv[++i], error);
		} else if (option == "--odd") {
			haveKey = descrambler.SetOddKey(argv[++i], error);
		} else if (option == "--pid") {
			onlyPid = (int)strtol(argv[++i], NULL, 16);
		} else {
			Usage(argv[0]);
			return 2;
		}
		if (!error.empty()) {
			fprintf(stderr, "%s\n", error.c_str());
			return 1;
		}
	}
	if (!haveKey) {
		fprintf(stderr, "at least one --even or --odd control word is required\n");
		return 2;
	}
	std::ifstream in(argv[1], std::ios::binary);
	std::ofstream out(argv[2], std::ios::binary);
	if (!in || !out) {
		fprintf(stderr, "could not open input or output\n");
		return 1;
	}
	uint8 packet[188];
	uint64 scrambledStarts = 0;
	uint64 validPesStarts = 0;
	while (in.read((char*)packet, sizeof(packet))) {
		int pid = ((packet[1] & 0x1f) << 8) | packet[2];
		int scrambling = packet[3] >> 6;
		bool checkStart = (packet[1] & 0x40) != 0
			&& (scrambling == 2 || scrambling == 3)
			&& (onlyPid < 0 || onlyPid == pid);
		if (checkStart)
			scrambledStarts++;
		descrambler.ProcessPacket(packet, onlyPid);
		if (checkStart) {
			int afc = (packet[3] >> 4) & 3;
			size_t pos = 4;
			if (afc == 3)
				pos += 1 + packet[4];
			if (pos + 3 <= 188 && packet[pos] == 0 && packet[pos + 1] == 0
				&& packet[pos + 2] == 1)
				validPesStarts++;
		}
		out.write((const char*)packet, sizeof(packet));
	}
	const TsDescrambler::stats& st = descrambler.Stats();
	printf("clear %llu, decrypted even %llu, odd %llu, missing key %llu, invalid %llu\n",
		(unsigned long long)st.clearPackets,
		(unsigned long long)st.decryptedEven,
		(unsigned long long)st.decryptedOdd,
		(unsigned long long)st.missingKey,
		(unsigned long long)st.invalidPackets);
	printf("decrypted PES starts: %llu/%llu\n",
		(unsigned long long)validPesStarts,
		(unsigned long long)scrambledStarts);
	bool keyPlausible = scrambledStarts == 0 || validPesStarts != 0;
	if (!keyPlausible)
		fprintf(stderr, "no PES start code survived: the supplied control word "
			"or scrambling algorithm is wrong\n");
	return st.missingKey == 0 && st.invalidPackets == 0 && keyPlausible ? 0 : 1;
}
