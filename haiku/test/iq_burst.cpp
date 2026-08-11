/*
 * One single enormous synchronous read, straight to a file.
 *
 * This exists because of a specific measured fact: at 2.048 MS/s the dongle
 * loses a few hundred samples per 96 ms frame on this machine, no matter which
 * API is used - synchronous reads measured 99.7-99.9% delivered, and async is
 * not better once the host has to keep up with 4.1 MB/s. The loss is invisible
 * in the byte count and it is silent, and for OFDM it is fatal: a DAB frame
 * still decodes (the samples inside it are contiguous), but the *next* frame
 * is a few hundred samples closer than the standard says it can be, so
 * anything that needs several consecutive frames - the time deinterleaver
 * needs sixteen consecutive CIFs, which is four frames - falls apart.
 *
 * The important half of that fact is that the loss happens *between* reads.
 * Within one transfer the samples are contiguous. So this asks for the whole
 * capture in a single rtlsdr_read_sync() call: however many frames fit in one
 * read are guaranteed to be continuous with each other.
 *
 *   iq_burst <MHz> <sample-rate> <MB> <gain-tenths|-1> <out.iq>
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include <OS.h>
#include <rtl-sdr.h>

int
main(int argc, char** argv)
{
	if (argc < 6) {
		fprintf(stderr, "usage: %s <MHz> <sample-rate> <MB> "
			"<gain-tenths|-1> <out.iq>\n", argv[0]);
		return 1;
	}

	double mhz = atof(argv[1]);
	uint32 rate = (uint32)strtoul(argv[2], NULL, 10);
	double megabytes = atof(argv[3]);
	int gain = atoi(argv[4]);
	const char* path = argv[5];

	// Must be a multiple of 512 for libusb bulk.
	size_t bytes = (size_t)(megabytes * 1024.0 * 1024.0);
	bytes &= ~(size_t)511;

	rtlsdr_dev_t* dev = NULL;
	if (rtlsdr_open(&dev, 0) < 0 || dev == NULL) {
		fprintf(stderr, "cannot open device\n");
		return 1;
	}
	rtlsdr_set_sample_rate(dev, rate);
	rtlsdr_set_agc_mode(dev, 0);
	if (gain < 0)
		rtlsdr_set_tuner_gain_mode(dev, 0);
	else {
		rtlsdr_set_tuner_gain_mode(dev, 1);
		rtlsdr_set_tuner_gain(dev, gain);
	}
	rtlsdr_set_center_freq(dev, (uint32)(mhz * 1e6 + 0.5));

	uint32 actual = rtlsdr_get_sample_rate(dev);
	fprintf(stderr, "tuned %.4f MHz, %u S/s, gain %d, one read of %lu bytes "
		"(%.3f s)\n", mhz, (unsigned)actual, gain, (unsigned long)bytes,
		(double)bytes / 2.0 / (double)actual);

	uint8* buffer = (uint8*)malloc(bytes);
	if (buffer == NULL) {
		fprintf(stderr, "cannot allocate %lu bytes\n", (unsigned long)bytes);
		return 1;
	}

	rtlsdr_reset_buffer(dev);
	// A throwaway read first: the transfer right after a buffer reset carries
	// whatever was already sitting in the FIFO.
	int junk = 0;
	rtlsdr_read_sync(dev, buffer, 65536, &junk);

	bigtime_t start = system_time();
	int got = 0;
	int err = rtlsdr_read_sync(dev, buffer, (int)bytes, &got);
	double elapsed = (double)(system_time() - start) / 1e6;

	fprintf(stderr, "read returned %d, %d bytes in %.3f s (%.1f%% of real "
		"time, so %.2f%% of samples arrived)\n", err, got, elapsed,
		100.0 * ((double)got / 2.0 / (double)actual) / elapsed,
		100.0 * ((double)got / 2.0 / (double)actual) / elapsed);

	if (got > 0) {
		FILE* out = fopen(path, "wb");
		if (out == NULL) {
			fprintf(stderr, "cannot write %s\n", path);
			return 1;
		}
		fwrite(buffer, 1, (size_t)got, out);
		fclose(out);
		fprintf(stderr, "wrote %s\n", path);
	}
	// No cancel, nothing queued - a synchronous read needs neither.
	rtlsdr_close(dev);
	return err < 0 ? 1 : 0;
}
