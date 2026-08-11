/*
 * Two questions about what librtlsdr and Haiku's libusb will tolerate, both
 * of which decide how the app has to be structured. Guessing at them produced
 * two wrong designs already.
 *
 *   1. Is a control transfer safe while a bulk stream is running?
 *      Setting the tuner gain from the demodulator thread stopped audio for
 *      eleven seconds and then killed the stream outright ("cb transfer
 *      status: 1, canceling..."). Is that reproducible, and does the
 *      frequency behave the same way as the gain?
 *
 *   2. Can a stream be cancelled and restarted repeatedly?
 *      If it can, retuning can stop the stream, change the setting and start
 *      again, which sidesteps question 1 entirely. If cancellation crashes
 *      libusb - as it does on process exit - then it cannot.
 *
 * Each phase prints the delivered sample rate, so "it kept working" and "it
 * silently stopped delivering" are distinguishable.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include <OS.h>
#include <rtl-sdr.h>

namespace {

const uint32 kRate = 264600;
const uint32 kFreq = 92500000;
const uint32 kBlockBytes = 32768;

struct counter {
	uint64			bytes;
	uint32			callbacks;
	volatile bool	stop;
};

counter gCounter;
rtlsdr_dev_t* gDevice = NULL;

void
Callback(unsigned char* buffer, uint32_t length, void* context)
{
	(void)buffer;
	(void)context;
	gCounter.bytes += length;
	gCounter.callbacks++;
	if (gCounter.stop)
		rtlsdr_cancel_async(gDevice);
}

status_t
StreamThread(void*)
{
	rtlsdr_reset_buffer(gDevice);
	int err = rtlsdr_read_async(gDevice, &Callback, NULL, 16, kBlockBytes);
	printf("    read_async returned %d\n", err);
	fflush(stdout);
	return B_OK;
}

// Delivered rate over the last interval, as a percentage of nominal.
double
Sample(bigtime_t seconds)
{
	uint64 before = gCounter.bytes;
	bigtime_t start = system_time();
	snooze(seconds * 1000000);
	double elapsed = (double)(system_time() - start) / 1e6;
	double rate = (double)(gCounter.bytes - before) / 2.0 / elapsed;
	return 100.0 * rate / (double)kRate;
}

} // namespace

int
main(int argc, char** argv)
{
	int phase = argc > 1 ? atoi(argv[1]) : 0;

	if (rtlsdr_open(&gDevice, 0) < 0 || gDevice == NULL) {
		fprintf(stderr, "cannot open device\n");
		return 1;
	}
	rtlsdr_set_sample_rate(gDevice, kRate);
	rtlsdr_set_tuner_gain_mode(gDevice, 1);
	rtlsdr_set_agc_mode(gDevice, 0);
	rtlsdr_set_tuner_gain(gDevice, 250);
	rtlsdr_set_center_freq(gDevice, kFreq);

	memset(&gCounter, 0, sizeof(gCounter));

	if (phase == 1) {
		printf("PHASE 1: control transfers during a running stream\n");
		thread_id t = spawn_thread(&StreamThread, "stream",
			B_URGENT_DISPLAY_PRIORITY, NULL);
		resume_thread(t);
		printf("  baseline            %6.1f%%\n", Sample(3));

		static const int kGains[] = { 197, 300, 350, 250 };
		for (int i = 0; i < 4; i++) {
			bigtime_t start = system_time();
			int err = rtlsdr_set_tuner_gain(gDevice, kGains[i]);
			double took = (double)(system_time() - start) / 1000.0;
			printf("  set_tuner_gain %3d   err %2d, took %7.1f ms, "
				"then %6.1f%%\n", kGains[i], err, took, Sample(2));
			fflush(stdout);
		}

		static const uint32 kFreqs[] = { 96700000, 92500000 };
		for (int i = 0; i < 2; i++) {
			bigtime_t start = system_time();
			int err = rtlsdr_set_center_freq(gDevice, kFreqs[i]);
			double took = (double)(system_time() - start) / 1000.0;
			printf("  set_center_freq %u  err %2d, took %7.1f ms, "
				"then %6.1f%%\n", (unsigned)kFreqs[i], err, took, Sample(2));
			fflush(stdout);
		}

		gCounter.stop = true;
		snooze(500000);
		printf("  callbacks total %u\n", (unsigned)gCounter.callbacks);
	} else if (phase == 2) {
		// Two candidate ways to retune, tried in the same run so that one
		// replug of the dongle answers both. A failed experiment leaves the
		// bulk endpoint wedged and only unplugging clears it, so each
		// strategy stops at its first failure rather than hammering on.
		printf("PHASE 2a: cancel, reset the FIFO, settle, then retune\n");
		bool aWorked = true;
		for (int round = 0; round < 3 && aWorked; round++) {
			gCounter.stop = false;
			thread_id t = spawn_thread(&StreamThread, "stream",
				B_URGENT_DISPLAY_PRIORITY, NULL);
			resume_thread(t);
			printf("  round %d: stream %6.1f%%\n", round + 1, Sample(2));
			fflush(stdout);

			gCounter.stop = true;
			status_t result;
			wait_for_thread(t, &result);

			// The RTL2832 does not stop producing samples just because
			// nobody is reading them, which is the suspected reason the
			// control path stays blocked after a cancel. Reset the FIFO and
			// give it real time to settle before trying a tuner write.
			int resetErr = rtlsdr_reset_buffer(gDevice);
			snooze(700000);
			int err = rtlsdr_set_center_freq(gDevice,
				(round % 2) ? kFreq : 96700000);
			printf("    reset_buffer %d, set_center_freq %d -> %s\n",
				resetErr, err, err == 0 ? "OK" : "FAILED");
			fflush(stdout);
			if (err != 0)
				aWorked = false;
		}
		printf("PHASE 2a verdict: %s\n\n",
			aWorked ? "retune after cancel works" : "retune after cancel FAILS");

		if (!aWorked) {
			// Fall back to the heavy option: close the device entirely, which
			// powers the baseband down, and open it again. Slow, but it is
			// the only sequence librtlsdr has that resets everything.
			printf("PHASE 2b: close and reopen the device for each retune\n");
			for (int round = 0; round < 3; round++) {
				rtlsdr_close(gDevice);
				gDevice = NULL;
				snooze(500000);

				if (rtlsdr_open(&gDevice, 0) < 0 || gDevice == NULL) {
					printf("  round %d: reopen FAILED\n", round + 1);
					break;
				}
				rtlsdr_set_sample_rate(gDevice, kRate);
				rtlsdr_set_tuner_gain_mode(gDevice, 1);
				rtlsdr_set_agc_mode(gDevice, 0);
				rtlsdr_set_tuner_gain(gDevice, 250);
				int err = rtlsdr_set_center_freq(gDevice,
					(round % 2) ? kFreq : 96700000);

				gCounter.stop = false;
				thread_id t = spawn_thread(&StreamThread, "stream",
					B_URGENT_DISPLAY_PRIORITY, NULL);
				resume_thread(t);
				printf("  round %d: set_center_freq %d, stream %6.1f%%\n",
					round + 1, err, Sample(2));
				fflush(stdout);
				gCounter.stop = true;
				status_t result;
				wait_for_thread(t, &result);
			}
		}
	} else {
		fprintf(stderr, "usage: %s <1|2>\n", argv[0]);
		return 1;
	}

	printf("reached the end without crashing\n");
	fflush(stdout);
	// Same reasoning as the app: do not let libusb unwind.
	_exit(0);
}
