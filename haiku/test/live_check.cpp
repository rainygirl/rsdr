/*
 * Headless run of the whole real-time chain: dongle -> Receiver -> Demodulator
 * -> AudioSink -> BSoundPlayer. Prints what it is doing once a second.
 *
 *   live_check 92.5 wfm 20
 *
 * A BApplication is created but never shown: the Media Kit refuses to hand
 * out a BSoundPlayer without one (BSoundPlayer talks to the media_server
 * through the app's own messenger), which is the one part of this that
 * cannot be tested without linking libbe.
 *
 * What to look at:
 *   dropped     USB blocks the demodulator could not keep up with. Anything
 *               other than 0 means the CPU is too slow for this mode.
 *   underruns   audio frames BSoundPlayer asked for and did not get.
 *   DSP load    fraction of real time spent demodulating. Above ~0.8 and the
 *               above two will start climbing.
 */
#include <Application.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "Receiver.h"

namespace {

demod_mode
ParseMode(const char* name)
{
	if (strcasecmp(name, "wfm") == 0)
		return kModeWFM;
	if (strcasecmp(name, "am") == 0)
		return kModeAM;
	if (strcasecmp(name, "lsb") == 0)
		return kModeLSB;
	if (strcasecmp(name, "usb") == 0)
		return kModeUSB;
	if (strcasecmp(name, "air") == 0)
		return kModeAir;
	if (strcasecmp(name, "dmb") == 0)
		return kModeDMB;
	return kModeCount;
}

class CheckApp : public BApplication {
public:
	CheckApp(uint64 frequency, demod_mode mode, int seconds, int ppm, int gain,
			int subchannel, int selectDelay)
		:
		BApplication("application/x-vnd.RSDR-livecheck"),
		fFrequency(frequency),
		fMode(mode),
		fSeconds(seconds),
		fPpm(ppm),
		fGain(gain),
		fSubchannel(subchannel),
		fSelectDelay(selectDelay)
	{
	}

	virtual void ReadyToRun()
	{
		Receiver receiver;
		receiver.SetPpm(fPpm);
		receiver.SetGain(fGain);
		receiver.SetMode(fMode);
		receiver.SetFrequency(fFrequency);
		receiver.SetVolume(0.7f);

		// Pick the service before starting, so the very first blocks feed the
		// intended subchannel rather than whichever one the FIC lists first.
		if (fSubchannel >= 0 && fSelectDelay <= 0)
			receiver.SetDabSubchannel(fSubchannel);
		status_t err = receiver.Start();
		if (err != B_OK) {
			Receiver::snapshot snap;
			receiver.Fetch(snap);
			fprintf(stderr, "start failed: %s (%s)\n", strerror(err),
				snap.status.c_str());
			Quit();
			return;
		}

		printf("device: %s\n", receiver.DeviceDescription().c_str());
		printf("tuned:  %.4f MHz  mode %s  ppm %d  gain %.1f dB%s\n",
			(double)fFrequency / 1e6, ModeName(fMode), fPpm, fGain / 10.0,
			fGain < 0 ? " (tuner auto)" : "");

		double peakSum = 0.0;
		int peakCount = 0;
		uint64 prevIq = 0;
		uint64 prevAudio = 0;
		uint64 prevUnder = 0;
		bigtime_t prevTime = system_time();

		for (int i = 0; i < fSeconds; i++) {
			snooze(1000000);
			if (fSubchannel >= 0 && fSelectDelay > 0
				&& i + 1 == fSelectDelay) {
				receiver.SetDabSubchannel(fSubchannel);
				printf("      selecting DMB subchannel %d after FIC probe\n",
					fSubchannel);
			}
			Receiver::snapshot snap;
			receiver.Fetch(snap);

			bigtime_t now = system_time();
			double dt = (double)(now - prevTime) / 1e6;
			prevTime = now;

			// The two rates that matter. iq/s is what the dongle actually
			// delivered - samples the RTL2832 dropped on the floor while no
			// read was in flight never appear here, so a value below the
			// nominal sample rate is the smoking gun for synchronous reads
			// not keeping up.
			double iqRate = dt > 0
				? (double)(snap.iqBytes - prevIq) / 2.0 / dt : 0.0;
			double audioRate = dt > 0
				? (double)(snap.audioSamples - prevAudio) / dt : 0.0;
			double underRate = dt > 0
				? (double)(snap.underruns - prevUnder) / dt : 0.0;
			prevIq = snap.iqBytes;
			prevAudio = snap.audioSamples;
			prevUnder = snap.underruns;

			printf("%2ds  RF %6.1f  gain %4.1f%s  peak %.3f  load %3.0f%%  "
				"iq %8.0f/s  audio %6.0f/s (want %u)  under %5.0f/s  "
				"drop %llu  off %+5.0f Hz\n",
				i + 1, snap.signalDb, snap.gainTenths / 10.0,
				snap.automaticGain ? "a" : " ",
				snap.audioPeak, snap.dspLoad * 100.0,
				iqRate, audioRate, (unsigned)snap.audioRate, underRate,
				(unsigned long long)snap.droppedBlocks,
				snap.carrierOffsetHz);
			if (snap.readErrors > 0 || iqRate < 1000.0) {
				printf("      readerr %llu  block %uK  devrate %u\n",
					(unsigned long long)snap.readErrors,
					(unsigned)(snap.blockBytes / 1024),
					(unsigned)snap.audioRate);
			}
			if (fMode == kModeDMB) {
				printf("      DAB %s  null %.3f  MER %4.1f dB  locks %u/%u"
					"  FIBs %u/%u  ensemble %s\n",
					snap.dabLocked ? "LOCK  " : "nolock",
					snap.dabNullDepth, snap.dabMerDb,
					(unsigned)snap.dabLocks, (unsigned)snap.dabAnalyses,
					(unsigned)snap.dabFibsOk, (unsigned)snap.dabFibsTried,
					snap.dabEnsemble.empty() ? "-" : snap.dabEnsemble.c_str());
				for (size_t j = 0; j < snap.dabServices.size(); j++)
					printf("        service: %s\n", snap.dabServices[j].c_str());
			}
			fflush(stdout);
			if (i >= 2) {
				peakSum += snap.audioPeak;
				peakCount++;
			}
		}

		Receiver::snapshot snap;
		receiver.Fetch(snap);
		double meanPeak = peakCount > 0 ? peakSum / peakCount : 0.0;
		printf("\nmean audio peak after settling: %.4f\n", meanPeak);
		printf("verdict: %s\n",
			meanPeak > 0.01 && snap.droppedBlocks == 0
				? "audio flowing, no dropped blocks"
				: (meanPeak <= 0.01 ? "NO AUDIO - check frequency/gain"
					: "audio present but blocks were dropped"));

		receiver.Stop();
		fflush(stdout);
		// _exit() rather than a clean unwind - see src/main.cpp.
		_exit(0);
	}

private:
	uint64		fFrequency;
	demod_mode	fMode;
	int			fSeconds;
	int			fPpm;
	int			fGain;
	int			fSubchannel;
	int			fSelectDelay;
};

} // namespace

int
main(int argc, char** argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <MHz> <wfm|nfm|am|lsb|usb> [seconds] "
			"[ppm] [gain-tenths-dB, -1 for tuner auto] [dmb-subchannel] "
			"[select-delay-sec]\n"
			"modes: wfm am air lsb usb dmb\n", argv[0]);
		return 1;
	}

	double mhz = atof(argv[1]);
	demod_mode mode = ParseMode(argv[2]);
	if (mode == kModeCount) {
		fprintf(stderr, "unknown mode %s\n", argv[2]);
		return 1;
	}
	int seconds = argc > 3 ? atoi(argv[3]) : 10;
	int ppm = argc > 4 ? atoi(argv[4]) : 0;
	// Tenths of a dB, or -1 for the tuner's own automatic mode.
	int gain = argc > 5 ? atoi(argv[5]) : Receiver::kDefaultGainTenths;

	int subchannel = argc > 6 ? atoi(argv[6]) : -1;
	int selectDelay = argc > 7 ? atoi(argv[7]) : 0;

	CheckApp app((uint64)(mhz * 1e6 + 0.5), mode, seconds, ppm, gain,
		subchannel, selectDelay);
	app.Run();
	return 0;
}
