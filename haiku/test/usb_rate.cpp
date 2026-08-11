/*
 * Everything this measures is about one question: how should the app read
 * from the dongle on Haiku, given that both obvious answers are broken?
 *
 *   rtlsdr_read_async()  delivers samples beautifully - 0.0045% lost - but it
 *                        can only be stopped with rtlsdr_cancel_async(), and
 *                        that reliably segfaults Haiku's libusb in
 *                        USBDeviceHandle::TransfersWorker(). A stream that
 *                        cannot be stopped also cannot be retuned, because a
 *                        control transfer issued while bulk transfers are
 *                        queued fails outright (measured: every
 *                        set_tuner_gain returned -1 after a ~2 s timeout, and
 *                        a few of those killed the stream).
 *
 *   rtlsdr_read_sync()   needs no cancellation - you simply stop calling it -
 *                        and leaves the bus idle between calls, which is when
 *                        a tuner write can get through. But it loses samples
 *                        in that same idle gap: 13% at 1058400 S/s with 32 KB
 *                        reads.
 *
 * The app now runs at 264600 S/s, a quarter of the rate those first
 * measurements were taken at. So:
 *
 *   phase 1  what fraction of the samples does a synchronous read actually
 *            deliver at 264600, against transfer size
 *   phase 2  does a tuner write between two synchronous reads succeed - i.e.
 *            can the app retune at all
 *
 * Neither phase uses the async API, so neither can trip the libusb crash.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <OS.h>
#include <rtl-sdr.h>

namespace {

uint32 gRate = 264600;
const uint32 kFreq = 92500000;

double
MeasureSerial(rtlsdr_dev_t* dev, size_t blockBytes, double seconds,
	int* errorsOut)
{
	std::vector<uint8> buf(blockBytes);
	rtlsdr_reset_buffer(dev);

	// One throwaway read: the first transfer after a reset carries whatever
	// was already in the FIFO and would flatter the result.
	int junk = 0;
	rtlsdr_read_sync(dev, &buf[0], (int)blockBytes, &junk);

	bigtime_t start = system_time();
	uint64 total = 0;
	int errors = 0;
	while ((double)(system_time() - start) / 1e6 < seconds) {
		int got = 0;
		if (rtlsdr_read_sync(dev, &buf[0], (int)blockBytes, &got) < 0) {
			errors++;
			break;
		}
		total += (uint64)got;
	}
	double elapsed = (double)(system_time() - start) / 1e6;
	*errorsOut = errors;
	if (elapsed <= 0.0)
		return 0.0;
	return (double)total / 2.0 / elapsed;
}

} // namespace

int
main(int argc, char** argv)
{
	int phase = argc > 1 ? atoi(argv[1]) : 1;
	if (argc > 2)
		gRate = (uint32)strtoul(argv[2], NULL, 10);

	rtlsdr_dev_t* dev = NULL;
	if (rtlsdr_open(&dev, 0) < 0 || dev == NULL) {
		fprintf(stderr, "cannot open device\n");
		return 1;
	}

	rtlsdr_set_sample_rate(dev, gRate);
	rtlsdr_set_tuner_gain_mode(dev, 1);
	rtlsdr_set_agc_mode(dev, 0);
	rtlsdr_set_tuner_gain(dev, 250);
	rtlsdr_set_center_freq(dev, kFreq);

	uint32 actual = rtlsdr_get_sample_rate(dev);
	printf("device reports %u S/s\n\n", (unsigned)actual);

	if (phase == 1) {
		printf("PHASE 1: synchronous read throughput at %u S/s\n",
			(unsigned)actual);
		printf("%9s %13s %11s %8s %10s\n", "block", "achieved S/s",
			"of nominal", "errors", "latency");

		static const size_t kSizes[] = {
			16384, 32768, 65536, 131072, 262144, 393216, 786432
		};
		for (size_t s = 0; s < sizeof(kSizes) / sizeof(kSizes[0]); s++) {
			int errors = 0;
			double achieved = MeasureSerial(dev, kSizes[s], 3.0, &errors);
			printf("%8luK %13.0f %10.1f%% %8d %8.0f ms\n",
				(unsigned long)(kSizes[s] / 1024), achieved,
				100.0 * achieved / (double)actual, errors,
				1000.0 * (double)kSizes[s] / 2.0 / (double)actual);
			fflush(stdout);
			snooze(200000);
		}
	} else if (phase == 2) {
		printf("PHASE 2: tuner writes between synchronous reads\n");
		const size_t kBlock = 65536;
		std::vector<uint8> buf(kBlock);
		rtlsdr_reset_buffer(dev);

		static const uint32 kFreqs[] = {
			96700000, 92500000, 102700000, 89100000, 92500000
		};
		static const int kGains[] = { 197, 250, 300, 250, 197 };

		for (int i = 0; i < 5; i++) {
			// A few reads, so the situation matches a running receiver.
			for (int k = 0; k < 4; k++) {
				int got = 0;
				rtlsdr_read_sync(dev, &buf[0], (int)kBlock, &got);
			}

			bigtime_t start = system_time();
			int freqErr = rtlsdr_set_center_freq(dev, kFreqs[i]);
			int gainErr = rtlsdr_set_tuner_gain(dev, kGains[i]);
			double took = (double)(system_time() - start) / 1000.0;

			// And confirm data still flows afterwards.
			rtlsdr_reset_buffer(dev);
			int got = 0;
			int readErr = rtlsdr_read_sync(dev, &buf[0], (int)kBlock, &got);

			printf("  %u Hz gain %d: freq %d, gain %d, took %6.1f ms, "
				"next read %d (%d bytes) -> %s\n",
				(unsigned)kFreqs[i], kGains[i], freqErr, gainErr, took,
				readErr, got,
				(freqErr == 0 && gainErr == 0 && readErr == 0 && got > 0)
					? "OK" : "FAILED");
			fflush(stdout);
		}
	} else {
		fprintf(stderr, "usage: %s <1|2> [sample-rate]\n", argv[0]);
		return 1;
	}

	printf("\nreached the end\n");
	rtlsdr_close(dev);
	return 0;
}
