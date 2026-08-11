/* Repeated tuner-control regression test for the macOS async capture path. */
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include "RtlDevice.h"

int
main(int argc, char** argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : 40;
	if (rounds <= 0)
		return 2;
	const uint32 frequencies[] = {
		603000, 711000, 972000, 3900000, 119100000, 92500000
	};
	const int frequencyCount = sizeof(frequencies) / sizeof(frequencies[0]);

	RtlDevice device;
	if (device.Open(0) != B_OK) {
		fprintf(stderr, "no RTL-SDR device\n");
		return 1;
	}
	device.RequestSampleRate(264600);
	device.RequestGain(402);
	device.RequestFrequency(frequencies[0]);
	if (device.Start() != B_OK)
		return 1;

	for (int i = 0; i < rounds; i++) {
		// This is the exact preset sequence that used to issue two cancels on
		// one async run: SetFrequency queued gain, then frequency immediately.
		device.RequestGain(402);
		device.RequestFrequency(frequencies[i % frequencyCount]);
		usleep(50000);
	}
	device.Stop();
	printf("%d repeated analog tuning changes: ok, %llu bytes captured\n", rounds,
		(unsigned long long)device.BytesRead());
	return 0;
}
