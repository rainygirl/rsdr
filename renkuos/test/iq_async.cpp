/*
 * Capture I/Q with rtlsdr_read_async and never cancel it.
 *
 * The problem this solves: at 2.048 MS/s this machine drops samples, and the
 * drops are not only between reads - a single 6 MB rtlsdr_read_sync() showed
 * them too, because libusb splits that one call into several URBs and the
 * device FIFO overflows in the gaps. A drop of a few hundred samples mid-frame
 * leaves the FIC readable (symbols 1-3) but wrecks the rate-2/3 MSC, and the
 * time deinterleaver needs sixteen consecutive CIFs, so four frames in a row
 * have to be clean.
 *
 * Queueing many URBs at once is exactly what async transfer is for: the device
 * always has somewhere to put the next block, so the FIFO never overflows.
 *
 * Async was avoided everywhere else in this project for one specific reason -
 * rtlsdr_cancel_async() segfaults inside Haiku's USBTransfer::Do(), which has
 * hard-wedged the dongle and taken the machine down more than once. A capture
 * tool does not need to cancel: fill the buffer, write the file, and _exit(0)
 * straight from the callback while transfers are still in flight. The kernel
 * tears the endpoint down with the process.
 *
 *   iq_async <MHz> <sample-rate> <MB> <gain-tenths|-1> <out.iq>
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include <OS.h>
#include <rtl-sdr.h>

static uint8*		sBuffer = NULL;
static size_t		sWanted = 0;
static size_t		sFilled = 0;
static const char*	sPath = NULL;
static bigtime_t	sStart = 0;
static uint32		sRate = 0;
static int			sBlocks = 0;

static void
finish()
{
	double elapsed = (double)(system_time() - sStart) / 1e6;
	double covered = (double)sFilled / 2.0 / (double)sRate;
	fprintf(stderr, "%lu bytes in %.3f s over %d callbacks; samples cover "
		"%.3f s, so %.2f%% of real time arrived\n", (unsigned long)sFilled,
		elapsed, sBlocks, covered, 100.0 * covered / elapsed);

	FILE* out = fopen(sPath, "wb");
	if (out != NULL) {
		fwrite(sBuffer, 1, sFilled, out);
		fflush(out);
		fclose(out);
		fprintf(stderr, "wrote %s\n", sPath);
	} else
		fprintf(stderr, "cannot write %s\n", sPath);

	// Deliberately no rtlsdr_cancel_async() and no rtlsdr_close(): both walk
	// into the crash described above. Let the kernel reclaim the endpoint.
	fflush(stderr);
	_exit(0);
}

static void
callback(unsigned char* buf, uint32_t len, void* ctx)
{
	sBlocks++;
	// The first block after streaming starts carries whatever was already in
	// the FIFO, so throw it away rather than splicing stale samples in.
	if (sBlocks == 1)
		return;

	size_t room = sWanted - sFilled;
	size_t take = (size_t)len < room ? (size_t)len : room;
	memcpy(sBuffer + sFilled, buf, take);
	sFilled += take;
	if (sFilled >= sWanted)
		finish();
}

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
	sPath = argv[5];

	sWanted = (size_t)(megabytes * 1024.0 * 1024.0) & ~(size_t)511;
	sBuffer = (uint8*)malloc(sWanted);
	if (sBuffer == NULL) {
		fprintf(stderr, "cannot allocate %lu bytes\n", (unsigned long)sWanted);
		return 1;
	}

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
	rtlsdr_reset_buffer(dev);

	sRate = rtlsdr_get_sample_rate(dev);
	fprintf(stderr, "tuned %.4f MHz, %u S/s, gain %d, want %lu bytes (%.3f s)\n",
		mhz, (unsigned)sRate, gain, (unsigned long)sWanted,
		(double)sWanted / 2.0 / (double)sRate);

	sStart = system_time();
	// 16 buffers of 256 KB: 4 MB of URBs in flight, about a second of slack.
	int err = rtlsdr_read_async(dev, callback, NULL, 16, 262144);
	fprintf(stderr, "read_async returned %d without filling the buffer\n", err);
	return 1;
}
