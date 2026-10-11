/*
 * Gap-free I/Q capture to a file, for offline analysis.
 *
 *   iq_capture <MHz> <sample-rate> <seconds> <gain-tenths|-1> <out.iq>
 *
 * This is the one place in the project that uses rtlsdr_read_async(), and it
 * has to: OFDM demodulation cannot tolerate missing samples at all, and
 * synchronous reads lose 13% of them at 1 MS/s (more above that). The async
 * path loses 45 per million.
 *
 * The catch with async is that stopping it means rtlsdr_cancel_async(), which
 * segfaults Haiku's libusb a moment later in USBDeviceHandle::TransfersWorker()
 * - see SdrDevice.h. So this never stops it: once enough bytes are written the
 * callback calls _exit() on the spot, before libusb has any chance to unwind.
 * Nothing needs flushing except the file, which is fflush'd first.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdlib>
#include <unistd.h>

#include <OS.h>
#include <rtl-sdr.h>

namespace {

rtlsdr_dev_t* gDevice = NULL;

// Captured into memory, written once at the end.
//
// Writing straight to a file loses samples: at 2.048 MS/s the stream is
// 4.1 MB/s and this machine's disk cannot absorb that from inside the USB
// callback, so buffers get dropped. The damage is invisible in the file size -
// the requested byte count still arrives - but the recording has gaps in it,
// and OFDM frame tracking falls apart a few frames in. Symptom that led here:
// the phase reference symbol correlated at 46x the noise floor for frames 0-2
// and then collapsed to 3x, with frame spacing jumping around by 11000
// samples, which no clock error can do.
uint8* gBuffer = NULL;
uint64 gWanted = 0;
uint64 gWritten = 0;
bigtime_t gStart = 0;
double gElapsed = 0.0;
volatile bool gDone = false;
const char* gPath = NULL;
uint32 gRate = 0;

void
Callback(unsigned char* buffer, uint32_t length, void* context)
{
	(void)context;
	if (gDone)
		return;

	size_t take = (size_t)length;
	if (gWritten + take > gWanted)
		take = (size_t)(gWanted - gWritten);

	memcpy(gBuffer + gWritten, buffer, take);
	gWritten += take;

	if (gWritten >= gWanted) {
		// Write and leave from inside the callback. NOT
		// rtlsdr_cancel_async() - that is the call that segfaults Haiku's
		// libusb, and putting it here (having documented exactly that
		// hazard elsewhere in this project) promptly crashed the machine.
		gElapsed = (double)(system_time() - gStart) / 1e6;
		gDone = true;
		FILE* out = fopen(gPath, "wb");
		if (out != NULL) {
			fwrite(gBuffer, 1, (size_t)gWritten, out);
			fclose(out);
		}
		fprintf(stderr, "captured %llu bytes in %.2f s (%.3f s of signal)\n",
			(unsigned long long)gWritten, gElapsed,
			(double)gWritten / 2.0 / (double)gRate);
		fflush(stderr);
		_exit(out != NULL ? 0 : 1);
	}
}

} // namespace

int
main(int argc, char** argv)
{
	if (argc < 6) {
		fprintf(stderr, "usage: %s <MHz> <sample-rate> <seconds> "
			"<gain-tenths|-1> <out.iq>\n", argv[0]);
		return 1;
	}

	double mhz = atof(argv[1]);
	uint32 rate = (uint32)strtoul(argv[2], NULL, 10);
	double seconds = atof(argv[3]);
	int gain = atoi(argv[4]);
	const char* path = argv[5];

	rtlsdr_dev_t*& dev = gDevice;
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

	uint32 actualRate = rtlsdr_get_sample_rate(dev);
	uint32 actualFreq = rtlsdr_get_center_freq(dev);
	fprintf(stderr, "tuned %u Hz, %u S/s, gain %d\n",
		(unsigned)actualFreq, (unsigned)actualRate, gain);

	gPath = path;
	gRate = actualRate;
	gWanted = (uint64)(seconds * (double)actualRate * 2.0);
	gBuffer = (uint8*)malloc((size_t)gWanted);
	if (gBuffer == NULL) {
		fprintf(stderr, "cannot allocate %llu bytes\n",
			(unsigned long long)gWanted);
		return 1;
	}
	gStart = system_time();

	rtlsdr_reset_buffer(dev);
	// 32 buffers of 128 KB: at 2.048 MS/s that is a second of slack, so a
	// scheduling hiccup on this single-core machine cannot cause a gap.
	rtlsdr_read_async(dev, &Callback, NULL, 32, 131072);

	// Only reached if the transfer failed before filling the request.
	fprintf(stderr, "read_async returned early after %llu bytes\n",
		(unsigned long long)gWritten);
	_exit(1);
}
