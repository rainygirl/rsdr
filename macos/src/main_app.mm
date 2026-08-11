/*
 * R SDR for macOS - Cocoa front end over the shared DSP core.
 *
 * Everything below the UI is the same code the Haiku app runs: Demodulator and
 * Dsp out of ../common, compiled unchanged. Only the three things that are
 * genuinely operating-system shaped are written twice - the dongle layer
 * (RtlDevice here, SdrDevice there), the audio sink (CoreAudio here,
 * BSoundPlayer there) and the window.
 *
 * The demodulator runs on its own thread and hands audio to the sink; the UI
 * polls a snapshot on a timer. Nothing in AppKit is touched from the worker.
 */
#import <Cocoa/Cocoa.h>

#include <pthread.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#import "VideoView.h"

#include "SupportDefs.h"
#include "AirBands.h"
#include "AirMonitor.h"
#include "Bands.h"
#include "CoreAudioSink.h"
#include "DabFic.h"
#include "DabMsc.h"
#include "DabSync.h"
#include "DmbAudio.h"
#include "Demodulator.h"
#include "Dsp.h"
#include "RtlDevice.h"
#include "TsDescrambler.h"

// ---------------------------------------------------------------- receiver

struct Snapshot {
	float	peak;
	float	signalDb;
	float	carrierOffset;
	bool	squelchOpen;
	uint64	underruns;
	uint64	dropped;
	double	iqRate;
	std::vector<float> spectrum;
	uint32	spectrumRate;
};

// DMB gain is an RF-condition property, not a station-name property. One
// 1.536-second FIC block is measured at each representative R820T gain and the
// CRC pass ratio chooses the setting. These values span weak-signal through
// overload territory without making initial station discovery excessively
// long.
static const int kDabProbeGains[] = { 166, 280, 328, 364, 402, 434, 496 };
static const int kDabProbeGainCount =
	(int)(sizeof(kDabProbeGains) / sizeof(kDabProbeGains[0]));

class MacReceiver {
public:
	MacReceiver()
		:
		fMode(kModeWFM),
		fFrequency(93100000ULL),
		// Analog default. DMB replaces this with an on-air FIB CRC sweep.
		fGain(402),
		fPpm(0),
		fSquelchDb(-200.0f),
		fFmStereo(false),
		fRunning(false),
		fThreadValid(false),
		fStopRequested(false),
		fPeak(0.0f),
		fSignalDb(-120.0f),
		fCarrierOffset(0.0f),
		fSquelchOpen(true),
		fIqRate(0.0),
		fSpectrumRate(0),
		fCityIndex(-1),
		fActiveChannel(-1),
		fDescramblerEnabled(false),
		fWantSubchannel(-1),
		fUsingSubchannel(-1),
		fDabResetRequested(false),
		fDabEpoch(0),
		fFicOnly(false),
		fFibsOk(0),
		fFibsTried(0),
		fDabFrames(0),
		fLastBlockBytes(0),
		fNullDepth(1.0f),
		fRfDb(-120.0f),
		fRsOk(0),
		fRsBlocks(0),
		fAudioUnits(0),
		fGainSweepActive(false),
		fGainSweepIndex(0),
		fGainSweepBestGain(328),
		fGainSweepBestScore(-1.0)
	{
		pthread_mutex_init(&fLock, NULL);
	}

	~MacReceiver()
	{
		Stop();
		pthread_mutex_destroy(&fLock);
	}

	bool IsRunning() const { return fRunning; }
	bool DeviceLost() const { return fDevice.DeviceLost(); }
	void CloseLostDevice() { fDevice.Close(); }
	const std::string& Description() const { return fDescription; }

	status_t Start()
	{
		if (fRunning)
			return B_OK;
		status_t err = fDevice.Open(0);
		if (err != B_OK)
			return err;
		fDescription = fDevice.Description();

		pthread_mutex_lock(&fLock);
		if (fMode == kModeDMB && fFicOnly) {
			fGainSweepActive = true;
			fGainSweepIndex = 0;
			fGainSweepBestGain = kDabProbeGains[0];
			fGainSweepBestScore = -1.0;
			fGainSweepBestServices.clear();
			fGain = kDabProbeGains[0];
			fDabResetRequested = true;
		}
		pthread_mutex_unlock(&fLock);

		mode_plan plan = PlanForMode(fMode);
		fDevice.RequestSampleRate(plan.tunerSampleRate);
		// Do not bake one dongle's measured correction into the application.
		// -84 ppm was right for the Haiku test receiver at 92.5 MHz, but on
		// the Mac live DAB path it produced FIB 0/2520 despite tracking 15/15
		// frames.  Zero keeps acquisition usable until correction is measured
		// for the particular receiver connected here.
		fDevice.RequestPpm(fPpm);
		fDevice.RequestGain(fGain);
		fDevice.RequestFrequency((uint32)fFrequency);
		err = fDevice.Start();
		if (err != B_OK)
			return err;

		fStopRequested = false;
		if (pthread_create(&fThread, NULL, &MacReceiver::_Entry, this) != 0)
			return B_ERROR;
		fThreadValid = true;
		fRunning = true;
		return B_OK;
	}

	void Stop()
	{
		if (!fRunning)
			return;
		fStopRequested = true;
		// Stop the USB producer first.  On unplug, the DSP thread can otherwise
		// remain in NextBlock() while this thread waits for it, leaving the UI in
		// "stopping" even after a replacement dongle has appeared.
		fDevice.Stop();
		if (fThreadValid) {
			pthread_join(fThread, NULL);
			fThreadValid = false;
		}
		fAudio.Shutdown();
		// Keep the librtlsdr handle across an ordinary Stop. macOS libusb
		// dispatches async completions on a separate hotplug run loop, and closing
		// here raced the last completion whenever an FIC-only station scan ended.
		// A physically lost device is closed explicitly after Stop has drained it.
		fRunning = false;
	}

	void SetMode(demod_mode mode)
	{
		pthread_mutex_lock(&fLock);
		if (mode == fMode) {
			pthread_mutex_unlock(&fLock);
			return;
		}
		fMode = mode;
		fCityIndex = -1;
		pthread_mutex_unlock(&fLock);
		// Release any block size the airband monitor forced. T-DMB runs at the
		// same 2.048 MS/s but needs whole 196608-sample frames; inheriting the
		// monitor's short blocks means the DAB path silently decodes nothing.
		fDevice.RequestBlockBytes(0);
		fDevice.RequestSampleRate(PlanForMode(mode).tunerSampleRate);
	}

	demod_mode Mode() const { return fMode; }

	void SetFrequency(uint64 hz)
	{
		pthread_mutex_lock(&fLock);
		bool frequencyChanged = hz != fFrequency;
		int wantedGain = fGain;
		if (fMode == kModeDMB) {
			std::map<uint64, int>::const_iterator saved = fDabGainCache.find(hz);
			wantedGain = saved != fDabGainCache.end() ? saved->second : 328;
		}
		bool gainChanged = wantedGain != fGain;
		if (hz != fFrequency) {
			// Do this synchronously with the preset change. Waiting for the next
			// 1.5-second IQ block let the UI repopulate Station with the previous
			// ensemble before DabFic::Reset() ran.
			fServiceLabels.clear();
			fServiceSubchannels.clear();
			fDabEpoch++;
		}
		fFrequency = hz;
		fGain = wantedGain;
		fDabResetRequested = true;
		pthread_mutex_unlock(&fLock);
		if (gainChanged)
			fDevice.RequestGain(wantedGain);
		if (frequencyChanged)
			fDevice.RequestFrequency((uint32)hz);
	}

	uint64 Frequency() const { return fFrequency; }

	void SetGain(int tenths)
	{
		fGain = tenths;
		fDevice.RequestGain(tenths);
	}

	void SetSquelchDb(float db)
	{
		pthread_mutex_lock(&fLock);
		fSquelchDb = db;
		pthread_mutex_unlock(&fLock);
	}

	void SetVolume(float v) { fAudio.SetVolume(v); }

	void SetFmStereo(bool enabled)
	{
		pthread_mutex_lock(&fLock);
		fFmStereo = enabled;
		pthread_mutex_unlock(&fLock);
	}

	// -1 leaves Air mode tuned to a single frequency; anything else monitors
	// that city's whole channel list at once.
	void SetCity(int index)
	{
		pthread_mutex_lock(&fLock);
		fCityIndex = index;
		if (index >= 0) {
			const air_city& city = AirCityAt(index);
			std::vector<AirMonitor::channel> chans;
			for (int i = 0; i < city.count; i++) {
				AirMonitor::channel c;
				c.label = city.channels[i].label;
				c.hz = city.channels[i].hz;
				chans.push_back(c);
			}
			fAir.SetChannels(chans);
			fAir.SetThresholdDb(3.0f);
		}
		fActiveChannel = -1;
		pthread_mutex_unlock(&fLock);

		if (index >= 0) {
			// Short blocks: at 2.048 MS/s the rate alone would pick DAB-sized
			// ones and put a second and a half of lag on every transmission.
			fDevice.RequestBlockBytes(262144);
			fDevice.RequestSampleRate(AirMonitor::kTunerRate);
			fDevice.RequestFrequency((uint32)fAir.TuneHz());
			fAir.RetuneDone();
		} else {
			fDevice.RequestBlockBytes(0);
			fDevice.RequestSampleRate(PlanForMode(fMode).tunerSampleRate);
			fDevice.RequestFrequency((uint32)fFrequency);
		}
	}

	int CityIndex() const { return fCityIndex; }

	void SetSubchannel(int sub)
	{
		pthread_mutex_lock(&fLock);
		fWantSubchannel = sub;
		pthread_mutex_unlock(&fLock);
	}

	void SetFicOnly(bool ficOnly)
	{
		pthread_mutex_lock(&fLock);
		fFicOnly = ficOnly;
		pthread_mutex_unlock(&fLock);
	}

	bool SetDmbControlWords(const std::string& even, const std::string& odd,
		std::string& error)
	{
		pthread_mutex_lock(&fLock);
		fDescrambler.ClearKeys();
		fDescramblerEnabled = false;
		bool ok = true;
		if (!even.empty()) {
			ok = fDescrambler.SetEvenKey(even, error);
			fDescramblerEnabled = ok;
		}
		if (ok && !odd.empty()) {
			ok = fDescrambler.SetOddKey(odd, error);
			fDescramblerEnabled = ok;
		}
		if (ok)
			fDemux.Reset();
		pthread_mutex_unlock(&fLock);
		return ok;
	}

	void Services(std::vector<std::string>& labels, std::vector<int>& subs)
	{
		pthread_mutex_lock(&fLock);
		labels = fServiceLabels;
		subs = fServiceSubchannels;
		pthread_mutex_unlock(&fLock);
	}

	// Video produced since the last call, moved out so the UI thread owns it.
	void TakeVideo(std::vector<uint8>& config,
		std::vector<DmbAudio::video_unit>& units)
	{
		pthread_mutex_lock(&fLock);
		config = fVideoConfig;
		units.swap(fVideoUnits);
		fVideoUnits.clear();
		pthread_mutex_unlock(&fLock);
	}

	bool AudioPlaybackPts(uint64& pts90k) const
	{
		return fAudio.PlaybackPts(pts90k);
	}

	void DabStats(uint64& rsOk, uint64& rsBlocks, uint64& audioUnits,
		int& subchannel, uint64& fibsOk, uint64& fibsTried, int& frames,
		int& blockKb, uint32& rate, float& nullDepth, float& rfDb, int& gain)
	{
		pthread_mutex_lock(&fLock);
		rsOk = fRsOk;
		rsBlocks = fRsBlocks;
		audioUnits = fAudioUnits;
		subchannel = fUsingSubchannel;
		fibsOk = fFibsOk;
		fibsTried = fFibsTried;
		frames = fDabFrames;
		blockKb = (int)(fLastBlockBytes / 1024);
		nullDepth = fNullDepth;
		rfDb = fRfDb;
		gain = fGain;
		rate = fDevice.SampleRate();
		pthread_mutex_unlock(&fLock);
	}

	void DescramblerStats(bool& enabled, uint64& valid, uint64& tried)
	{
		pthread_mutex_lock(&fLock);
		enabled = fDescramblerEnabled;
		const TsDescrambler::stats& st = fDescrambler.Stats();
		valid = st.validPesStarts;
		tried = st.scrambledPesStarts;
		pthread_mutex_unlock(&fLock);
	}

	// Loudest channel in the window right now, however far below the
	// threshold, so the display can show that something is being heard even
	// when the squelch stays shut.
	void BestChannel(std::string& label, float& overFloor)
	{
		label.clear();
		overFloor = 0.0f;
		pthread_mutex_lock(&fLock);
		const std::vector<AirMonitor::channel>& ch = fAir.Channels();
		int w = fAir.CurrentWindow();
		float best = -1e9f;
		int bestIndex = -1;
		if (w >= 0 && w < fAir.WindowCount()) {
			for (size_t i = 0; i < ch.size(); i++) {
				float over = ch[i].levelDb - ch[i].floorDb;
				if (ch[i].levelDb <= -139.0f)
					continue;
				if (over > best) {
					best = over;
					bestIndex = (int)i;
				}
			}
		}
		if (bestIndex >= 0) {
			label = ch[bestIndex].label;
			overFloor = best;
		}
		pthread_mutex_unlock(&fLock);
	}

	std::string ActiveChannelLabel()
	{
		pthread_mutex_lock(&fLock);
		std::string s;
		int a = fActiveChannel;
		if (a >= 0 && a < (int)fAir.Channels().size()) {
			char buf[128];
			snprintf(buf, sizeof(buf), "%s  %.3f MHz",
				fAir.Channels()[a].label.c_str(),
				fAir.Channels()[a].hz / 1e6);
			s = buf;
		}
		pthread_mutex_unlock(&fLock);
		return s;
	}

	void Fetch(Snapshot& out)
	{
		pthread_mutex_lock(&fLock);
		out.peak = fPeak;
		out.signalDb = fSignalDb;
		out.carrierOffset = fCarrierOffset;
		out.squelchOpen = fSquelchOpen;
		out.spectrum = fSpectrum;
		out.spectrumRate = fSpectrumRate;
		out.iqRate = fIqRate;
		pthread_mutex_unlock(&fLock);
		out.underruns = fAudio.Underruns();
		out.dropped = fDevice.DroppedBlocks();
	}

private:
	static void* _Entry(void* c)
	{
		static_cast<MacReceiver*>(c)->_Loop();
		return NULL;
	}

	void _Loop()
	{
		std::vector<uint8> block;
		std::vector<float> audio;
		uint32 generation = 0;
		uint32 configuredGeneration = 0xffffffff;
		bool configuredStereo = false;
		bigtime_t lastRate = system_time();
		uint64 lastBytes = fDevice.BytesRead();

		while (!fStopRequested) {
			if (!fDevice.NextBlock(block, generation, 250000))
				continue;
			if (generation != fDevice.Generation())
				continue;

			demod_mode mode;
			float squelch;
			bool fmStereo;
			pthread_mutex_lock(&fLock);
			mode = fMode;
			squelch = fSquelchDb;
			fmStereo = fFmStereo;
			pthread_mutex_unlock(&fLock);

			if (mode == kModeDMB) {
				if (generation != configuredGeneration) {
					// Start with the common 48 kHz value; _ProcessDab switches to
					// the exact rate reported by each service's BSAC config as soon
					// OD arrives. DAB blocks need the burst-sized ring either way.
					fAudio.Configure(48000, true);
					configuredGeneration = generation;
				}
				_ProcessDab(block);
				continue;
			}

			int city;
			pthread_mutex_lock(&fLock);
			city = fCityIndex;
			pthread_mutex_unlock(&fLock);

			if (city >= 0) {
				// City monitoring: one FFT finds every active channel in the
				// window, and only the one that is transmitting is demodulated.
				if (generation != configuredGeneration) {
					fAudio.Configure(AirMonitor::kAudioRate);
					configuredGeneration = generation;
				}
				// The slider runs -100..0; map it onto a 3..28 dB margin over
				// the measured noise floor. 3 dB at the bottom is deliberately
				// permissive - it lets weak traffic through at the cost of
				// occasional noise, which is the right trade when the
				// alternative is hearing nothing at all.
				float margin = squelch <= -190.0f ? 3.0f
					: 3.0f + (squelch + 100.0f) * 0.25f;
				pthread_mutex_lock(&fLock);
				fAir.SetThresholdDb(margin);
				pthread_mutex_unlock(&fLock);

				audio.clear();
				int active = -1;
				fAir.Process(&block[0], block.size(), audio, active);
				if (!audio.empty())
					fAudio.Write(&audio[0], audio.size());
				if (fAir.NeedsRetune()) {
					fDevice.RequestFrequency((uint32)fAir.TuneHz());
					fAir.RetuneDone();
				}
				pthread_mutex_lock(&fLock);
				fActiveChannel = active;
				fPeak = fAudio.PeakLevel();
				fSquelchOpen = active >= 0;
				pthread_mutex_unlock(&fLock);
				continue;
			}

			bool useStereo = mode == kModeWFM && fmStereo;
			if (generation != configuredGeneration
					|| useStereo != configuredStereo) {
				uint32 rate = fDevice.SampleRate();
				if (rate == 0)
					continue;
				fDemod.Configure(mode, rate);
				fDemod.SetFmStereo(useStereo);
				fAudio.Configure(PlanForMode(mode).audioSampleRate, false,
					useStereo ? 2 : 1);
				configuredGeneration = generation;
				configuredStereo = useStereo;
			}
			fDemod.SetSquelch(squelch);

			size_t maxOut = fDemod.MaxAudioSamples(block.size());
			if (audio.size() < maxOut)
				audio.resize(maxOut);
			size_t produced = fDemod.Process(&block[0], block.size(),
				&audio[0], audio.size());
			if (produced > 0) {
				if (useStereo)
					fAudio.WriteStereo(&audio[0], produced / 2);
				else
					fAudio.Write(&audio[0], produced);
			}

			// Spectrum for the display, from the filtered baseband.
			const std::vector<dsp::Cf>& bb = fDemod.LastBaseband();
			std::vector<float> mags;
			// 2048 bins rather than 512: at 264.6 kHz across the window that
			// is 129 Hz a bin instead of 517, which is the difference between
			// a smear and seeing individual carriers.
			int bins = bb.size() >= 2048 ? 2048 : (bb.size() >= 1024 ? 1024 : 0);
			if (bins > 0) {
				mags.resize(bins);
				dsp::PowerSpectrumDb(&bb[0], bb.size(), bins, mags);
			}

			bigtime_t now = system_time();
			double iqRate = fIqRate;
			if (now - lastRate >= 500000) {
				uint64 bytes = fDevice.BytesRead();
				iqRate = (double)(bytes - lastBytes) / 2.0
					/ ((double)(now - lastRate) / 1e6);
				lastBytes = bytes;
				lastRate = now;
			}

			pthread_mutex_lock(&fLock);
			fPeak = fAudio.PeakLevel();
			fSignalDb = fDemod.SignalLevelDb();
			fCarrierOffset = fDemod.CarrierOffsetHz();
			fSquelchOpen = fDemod.SquelchOpen();
			fIqRate = iqRate;
			if (!mags.empty()) {
				fSpectrum = mags;
				fSpectrumRate = fDemod.BasebandRate();
			}
			pthread_mutex_unlock(&fLock);
		}
	}

	void _ProcessDab(const std::vector<uint8>& block)
	{
		bool resetRequested;
		bool ficOnly;
		uint64 processEpoch;
		pthread_mutex_lock(&fLock);
		resetRequested = fDabResetRequested;
		fDabResetRequested = false;
		ficOnly = fFicOnly;
		processEpoch = fDabEpoch;
		pthread_mutex_unlock(&fLock);
		if (resetRequested) {
			fFic.Reset();
			fDemux.Reset();
			fDabCarry.clear();
			fUsingSubchannel = -1;
			pthread_mutex_lock(&fLock);
			fServiceLabels.clear();
			fServiceSubchannels.clear();
			fVideoConfig.clear();
			fVideoUnits.clear();
			pthread_mutex_unlock(&fLock);
		}

		fLastBlockBytes = block.size();
		size_t count = block.size() / 2;
		if (count < (size_t)DabSync::kFrameSamples)
			return;
		// A USB read is exactly sixteen DAB frame periods but begins at an
		// arbitrary point in a frame. Preserve the unfinished frame at the end
		// and prepend it here; otherwise one real frame was replaced by four
		// erasure CIFs every 1.536 seconds, heard and seen as the periodic tick.
		size_t carry = fDabCarry.size();
		fDabIq.resize(carry + count);
		if (carry > 0)
			memcpy(&fDabIq[0], &fDabCarry[0], carry * sizeof(dsp::Cf));
		for (size_t i = 0; i < count; i++) {
			fDabIq[carry + i].re = ((float)block[2 * i] - 127.4f)
				* (1.0f / 128.0f);
			fDabIq[carry + i].im = ((float)block[2 * i + 1] - 127.4f)
				* (1.0f / 128.0f);
		}
		count += carry;
		fDabCarry.clear();
		fSync.SetBuffer(&fDabIq[0], count);

		size_t need = (size_t)(DabSync::kSymbolsPerFrame - 1)
			* DabSync::kSoftPerSymbol;
		if (fDabSoft.size() < need)
			fDabSoft.resize(need);
		if (fDabPos.size() < (size_t)DabSync::kSymbolsPerFrame)
			fDabPos.resize(DabSync::kSymbolsPerFrame);

		// Mean power, so "no signal" can be told from "signal but not DAB".
		double power = 0.0;
		for (size_t i = 0; i < count; i += 64) {
			power += (double)fDabIq[i].re * fDabIq[i].re
				+ (double)fDabIq[i].im * fDabIq[i].im;
		}
		float rfDb = 10.0f * log10f((float)(power / (count / 64)) + 1e-20f);

		// A wide USB block is exactly sixteen DAB frame periods. Usually only
		// fifteen frames are complete because one straddles the read boundary.
		// Keep the MSC interleaver alive across reads and represent that missing
		// frame with four erasure CIFs. Resetting here did produce perfect RS
		// blocks, but it also discarded the 16-CIF priming interval every 1.536
		// seconds: measured RS 241/241 with visibly periodic video freezes and
		// clicking BSAC audio. Preserving the timeline removes that self-made
		// outage.

		size_t searchFrom = 0;
		int expectedNext = -1;
		int tracked = 0;
		float firstDepth = 1.0f;
		bool haveDepth = false;
		std::vector<float> pcm;
		static std::vector<float> erasure(DabMsc::kCifBits, 0.0f);

		while (true) {
			float fineHz = 0.0f;
			float depth = 1.0f;
			int start = -1;
			int attemptedExpected = expectedNext;
			if (expectedNext >= 0)
				start = fSync.NextFrame(expectedNext, &fineHz);
			if (start < 0)
				start = fSync.FindFrame(searchFrom, &fineHz, &depth);
			if (!haveDepth) {
				firstDepth = depth;
				haveDepth = true;
			}
			if (start < 0) {
				// The next null/partial frame belongs to the next USB block. Keep
				// enough lead-in for FindFrame's null-power window.
				if (attemptedExpected >= 0) {
					size_t from = attemptedExpected > DabSync::kNullSamples + 512
						? (size_t)(attemptedExpected - DabSync::kNullSamples - 512)
						: 0;
					if (from < count)
						fDabCarry.assign(fDabIq.begin() + from, fDabIq.end());
				} else {
					size_t keep = (size_t)DabSync::kFrameSamples
						+ DabSync::kNullSamples + 512;
					size_t from = count > keep ? count - keep : 0;
					fDabCarry.assign(fDabIq.begin() + from, fDabIq.end());
				}
				break;
			}

			// NextFrame can identify a frame from its first twelve symbols. Do
			// not call TrackFrame and turn the missing tail into an erasure; carry
			// this exact frame forward until all 76 symbols are present.
			if ((size_t)start + (size_t)DabSync::kSymbolsPerFrame
					* DabSync::kSymbolSamples > count) {
				size_t from = start > DabSync::kNullSamples + 512
					? (size_t)(start - DabSync::kNullSamples - 512) : 0;
				fDabCarry.assign(fDabIq.begin() + from, fDabIq.end());
				break;
			}
			searchFrom = (size_t)start
				+ (size_t)(DabSync::kSymbolsPerFrame - 2)
					* DabSync::kSymbolSamples;

			if (!fSync.TrackFrame(start, fineHz, &fDabPos[0])) {
				expectedNext = -1;
				// The frame still occupied its 96 ms, so its four CIFs have to
				// be accounted for. Skipping them shifts the 16-CIF
				// interleaver by four and every later logical frame is then
				// assembled from the wrong CIFs.
				if (!ficOnly && fMsc.Configured()) {
					for (int c = 0; c < 4; c++)
						fMsc.PushCif(&erasure[0]);
				}
				continue;
			}
			expectedNext = fDabPos[DabSync::kSymbolsPerFrame - 1]
				+ DabSync::kSymbolSamples + DabSync::kNullSamples;
			tracked++;
			fSync.SoftBits(&fDabPos[0], fineHz, &fDabSoft[0]);
			fFic.DecodeFrame(&fDabSoft[0]);
			if (ficOnly)
				continue;

			int want;
			pthread_mutex_lock(&fLock);
			want = fWantSubchannel;
			pthread_mutex_unlock(&fLock);

			if (want >= 0 && (!fMsc.Configured() || fUsingSubchannel < 0
				|| want != fUsingSubchannel)) {
				const std::vector<DabFic::subchannel>& subs =
					fFic.Subchannels();
				for (size_t i = 0; i < subs.size(); i++) {
					if (subs[i].shortForm || subs[i].sizeCu <= 0)
						continue;
					if (want >= 0 && subs[i].id != want)
						continue;
					DabMsc::config c;
					c.startCu = subs[i].startCu;
					c.sizeCu = subs[i].sizeCu;
					c.protectionLevel = subs[i].protectionLevel;
					c.eepOptionB = subs[i].eepOptionB;
					if (fMsc.Configure(c)) {
						fUsingSubchannel = subs[i].id;
						fDemux.Reset();
						break;
					}
				}
			}

			if (want >= 0 && fMsc.Configured()) {
				const float* msc = &fDabSoft[(size_t)3
					* DabSync::kSoftPerSymbol];
				for (int c = 0; c < 4; c++)
					fMsc.PushCif(msc + (size_t)c * DabMsc::kCifBits);
				const std::vector<uint8>& packets = fMsc.Packets();
				if (!packets.empty()) {
					const uint8* feed = &packets[0];
					std::vector<uint8> clearPackets;
					pthread_mutex_lock(&fLock);
					if (fDescramblerEnabled) {
						clearPackets = packets;
						for (size_t p = 0; p + 188 <= clearPackets.size(); p += 188)
							fDescrambler.ProcessPacket(&clearPackets[p]);
						feed = &clearPackets[0];
					}
					pthread_mutex_unlock(&fLock);
					fDemux.Feed(feed, packets.size(), pcm);
					fMsc.ClearPackets();
				}
			}
		}

		// Sample rate belongs to the selected service, not the ensemble. MBC's
		// measured ASC is 44.1 kHz while this SBS service produces exactly the
		// 73,728 frames per 1.536-second block expected at 48 kHz. Playing the
		// latter through a hard-coded 44.1 kHz queue accumulated 8.8% each block
		// until the ring dropped old samples with a periodic "chik".
		int decodedRate = fDemux.Stats().sampleRate;
		if (decodedRate > 0 && fAudio.SampleRate() != (uint32)decodedRate)
			fAudio.Configure((uint32)decodedRate, true);
		std::vector<DmbAudio::audio_anchor> anchors;
		fDemux.TakeAudioAnchors(anchors);
		if (!pcm.empty()) {
			if (anchors.empty()) {
				fAudio.Write(&pcm[0], pcm.size());
			} else {
				size_t cursor = 0;
				for (size_t i = 0; i < anchors.size(); i++) {
					size_t start = anchors[i].sampleOffset;
					if (start > pcm.size())
						continue;
					if (start > cursor)
						fAudio.Write(&pcm[cursor], start - cursor);
					size_t end = i + 1 < anchors.size()
						? anchors[i + 1].sampleOffset : pcm.size();
					if (end > pcm.size())
						end = pcm.size();
					if (end > start)
						fAudio.WriteTimed(&pcm[start], end - start,
							anchors[i].pts90k);
					cursor = end;
				}
				if (cursor < pcm.size())
					fAudio.Write(&pcm[cursor], pcm.size() - cursor);
			}
		}

		std::vector<DmbAudio::video_unit> units;
		DmbAudio::video_unit unit;
		while (fDemux.TakeVideoUnit(unit))
			units.push_back(unit);

		// During FIC-only startup, measure one complete block at each generic gain.
		// Publish no Station list until all candidates have been compared; otherwise
		// the UI would stop the probe after the first merely-decodable setting.
		std::vector<DabFic::service> services = fFic.Services();
		int applyGain = -1;
		if (ficOnly) {
			pthread_mutex_lock(&fLock);
			if (fGainSweepActive) {
				double ratio = fFic.FibsTried() > 0
					? (double)fFic.FibsOk() / (double)fFic.FibsTried() : 0.0;
				// CRC ratio dominates. Null depth breaks close ties and a fuller
				// service list weakly favours a block that saw all FIGs.
				double score = ratio * 1000.0 - firstDepth * 10.0
					+ services.size() * 0.01;
				int testedGain = kDabProbeGains[fGainSweepIndex];
				if (score > fGainSweepBestScore) {
					fGainSweepBestScore = score;
					fGainSweepBestGain = testedGain;
					fGainSweepBestServices = services;
				}
				fGainSweepIndex++;
				if (fGainSweepIndex < kDabProbeGainCount) {
					applyGain = kDabProbeGains[fGainSweepIndex];
					fGain = applyGain;
					fDabResetRequested = true;
					services.clear();
				} else {
					fGainSweepActive = false;
					fGain = fGainSweepBestGain;
					fDabGainCache[fFrequency] = fGainSweepBestGain;
					applyGain = fGainSweepBestGain;
					services = fGainSweepBestServices;
				}
			}
			pthread_mutex_unlock(&fLock);
			if (applyGain >= 0)
				fDevice.RequestGain(applyGain);
		}

		std::vector<std::string> labels;
		std::vector<int> subs;
		const std::vector<DabFic::service>& svc = services;
		for (size_t i = 0; i < svc.size(); i++) {
			// Publish the FIC service list as soon as label and SubChId are
			// known. FIG 0/3 (DSCTy, including the usual value 24) can arrive
			// later than FIG 0/2 and FIG 1; waiting for it made a perfectly
			// decoded 8B ensemble stay at "Station..." indefinitely.
			if (svc[i].label.empty() || svc[i].subchannel < 0) {
				continue;
			}
			labels.push_back(svc[i].label
				+ (svc[i].conditionalAccess ? " (encrypted)" : ""));
			subs.push_back(svc[i].subchannel);
		}

		pthread_mutex_lock(&fLock);
		// SetFrequency can run on the main thread while this 1.536-second old
		// block is still being decoded. Never republish that old ensemble after
		// resetStations() has cleared it, nor cache it under the new preset.
		if (processEpoch == fDabEpoch) {
			fServiceLabels = labels;
			fServiceSubchannels = subs;
			fVideoConfig = fDemux.VideoConfig();
			for (size_t i = 0; i < units.size(); i++)
				fVideoUnits.push_back(units[i]);
			if (fVideoUnits.size() > 120) {
				fVideoUnits.erase(fVideoUnits.begin(),
					fVideoUnits.begin() + (fVideoUnits.size() - 120));
			}
		}
		// One line a block to /tmp/rsdr-dab.log, and only if that file already
		// exists - `touch` it to turn this on. The window cannot be read from
		// a terminal, and this path behaves differently inside the app than the
		// identical standalone code, which is exactly the kind of thing a
		// status line is too slow to catch.
		{
			static FILE* log = NULL;
			static bool checked = false;
			if (!checked) {
				checked = true;
				FILE* probe = fopen("/tmp/rsdr-dab.log", "r");
				if (probe != NULL) {
					fclose(probe);
					log = fopen("/tmp/rsdr-dab.log", "w");
				}
			}
			if (log != NULL) {
				const DmbAudio::stats& audioStats = fDemux.Stats();
				int cfgSize = 0, cfgLevel = 0, cfgOption = 0;
				const std::vector<DabFic::subchannel>& logSubs =
					fFic.Subchannels();
				for (size_t si = 0; si < logSubs.size(); si++) {
					if (logSubs[si].id == fUsingSubchannel) {
						cfgSize = logSubs[si].sizeCu;
						cfgLevel = logSubs[si].protectionLevel;
						cfgOption = logSubs[si].eepOptionB ? 1 : 0;
						break;
					}
				}
				fprintf(log, "block %lu  gain %.1f  RF %.1f  null %.3f  tracked %d  "
					"fibs %llu/%llu  subch %d cfg %d  cifs %llu frames %llu "
					"synced %d blocks %llu rsok %llu  pcm %lu  "
					"eep %c%d cu %d kbps %d  arate %d aerr %llu "
					"aq %lu under %llu over %llu  rms %.3f diff %.3f "
					"jump %.3f loud %llu conceal %llu\n",
					(unsigned long)block.size(), fGain / 10.0f, rfDb, firstDepth,
					tracked,
					(unsigned long long)fFic.FibsOk(),
					(unsigned long long)fFic.FibsTried(),
					fUsingSubchannel, fMsc.Configured() ? 1 : 0,
					(unsigned long long)fMsc.Stats().cifsIn,
					(unsigned long long)fMsc.Stats().framesOut,
					fMsc.Stats().synced ? 1 : 0,
					(unsigned long long)fMsc.Stats().blocksTried,
					(unsigned long long)fMsc.Stats().rsOk,
					(unsigned long)pcm.size(), cfgOption ? 'B' : 'A', cfgLevel,
					cfgSize, fMsc.Stats().bitrateKbps, audioStats.sampleRate,
					(unsigned long long)audioStats.errors,
					(unsigned long)fAudio.QueuedFrames(),
					(unsigned long long)fAudio.Underruns(),
					(unsigned long long)fAudio.Overruns(),
					audioStats.maxUnitRms, audioStats.maxDifferenceRms,
					audioStats.maxBoundaryJump,
					(unsigned long long)audioStats.loudUnits,
					(unsigned long long)audioStats.concealedUnits);
				fflush(log);
			}
		}

		fNullDepth = firstDepth;
		fRfDb = rfDb;
		fFibsOk = fFic.FibsOk();
		fFibsTried = fFic.FibsTried();
		fDabFrames += tracked;
		fRsOk = fMsc.Stats().rsOk;
		fRsBlocks = fMsc.Stats().blocksTried;
		fAudioUnits = fDemux.Stats().decodedUnits;
		fPeak = fAudio.PeakLevel();
		pthread_mutex_unlock(&fLock);
	}

	RtlDevice		fDevice;
	CoreAudioSink	fAudio;
	Demodulator		fDemod;
	std::string		fDescription;

	demod_mode		fMode;
	uint64			fFrequency;
	int				fGain;
	int				fPpm;
	float			fSquelchDb;
	bool			fFmStereo;

	bool			fRunning;
	pthread_t		fThread;
	bool			fThreadValid;
	volatile bool	fStopRequested;

	mutable pthread_mutex_t	fLock;
	float			fPeak;
	float			fSignalDb;
	float			fCarrierOffset;
	bool			fSquelchOpen;
	double			fIqRate;
	std::vector<float>	fSpectrum;
	uint32			fSpectrumRate;
	AirMonitor		fAir;
	int				fCityIndex;
	int				fActiveChannel;

	// T-DMB. The OFDM-to-transport-stream half is the shared code out of
	// ../common; only the video sink below it is platform specific.
	DabSync			fSync;
	DabFic			fFic;
	DabMsc			fMsc;
	DmbAudio		fDemux;
	TsDescrambler	fDescrambler;
	bool			fDescramblerEnabled;
	std::vector<dsp::Cf>	fDabIq;
	std::vector<dsp::Cf>	fDabCarry;
	std::vector<float>		fDabSoft;
	std::vector<int>		fDabPos;
	int				fWantSubchannel;
	int				fUsingSubchannel;
	bool			fDabResetRequested;
	uint64			fDabEpoch;
	bool			fFicOnly;
	std::vector<std::string>	fServiceLabels;
	std::vector<int>			fServiceSubchannels;
	std::vector<uint8>			fVideoConfig;
	std::vector<DmbAudio::video_unit>	fVideoUnits;
	uint64			fFibsOk;
	uint64			fFibsTried;
	int				fDabFrames;
	size_t			fLastBlockBytes;
	float			fNullDepth;
	float			fRfDb;
	uint64			fRsOk;
	uint64			fRsBlocks;
	uint64			fAudioUnits;

	bool			fGainSweepActive;
	int			fGainSweepIndex;
	int			fGainSweepBestGain;
	double			fGainSweepBestScore;
	std::vector<DabFic::service> fGainSweepBestServices;
	std::map<uint64, int> fDabGainCache;
};

static MacReceiver gReceiver;

// ------------------------------------------------------------- spectrum view

// Noise floor is blue and stays blue: the ramp only leaves the blues once a
// bin is 25% of the way up the 45 dB span above the floor, so an empty band
// reads as a flat blue field rather than a cyan one.
static void
WaterfallColour(float t, CGFloat& r, CGFloat& g, CGFloat& b)
{
	if (t < 0.25f) {
		// near-black to deep blue
		float u = t / 0.25f;
		r = 0.0; g = 0.0; b = 0.20 + 0.55 * u;
	} else if (t < 0.5f) {
		// deep blue to blue-cyan
		float u = (t - 0.25f) / 0.25f;
		r = 0.0; g = 0.45 * u; b = 0.75 + 0.25 * u;
	} else if (t < 0.75f) {
		// cyan to yellow
		float u = (t - 0.5f) / 0.25f;
		r = u; g = 0.45 + 0.55 * u; b = 1.0 - u;
	} else {
		// yellow to white
		float u = (t - 0.75f) / 0.25f;
		r = 1.0; g = 1.0; b = u;
	}
}

@interface SpectrumView : NSView
- (void)pushBins:(const std::vector<float>&)bins spanHz:(uint32)spanHz
	centerHz:(uint64)centerHz mode:(demod_mode)mode;
- (void)setTuneTarget:(id)target action:(SEL)action;
- (float)clickedOffsetHz;
@end

@implementation SpectrumView {
	std::vector<float> _bins;
	std::vector<uint8_t> _fall;   // waterfall, RGBA per cell, drawn as one image
	int _fallWidth;
	int _fallRows;
	int _fallHead;
	float _floorDb;
	std::vector<uint8_t> _ordered;
	uint32 _spanHz;
	uint64 _centerHz;
	demod_mode _displayMode;
	float _clickedOffsetHz;
	NSTrackingArea* _trackingArea;
	NSPoint _hoverPoint;
	bool _hovering;
	id _tuneTarget;
	SEL _tuneAction;
}

- (instancetype)initWithFrame:(NSRect)frame
{
	self = [super initWithFrame:frame];
	_fallWidth = 2048;
	// Keep one stored row per displayed point.  Scaling 256 history rows into
	// the 160-point waterfall made neighbouring rows alternately grow, shrink
	// and disappear as the ring advanced, which looked like a shimmering
	// resize even though the view itself never changed size.
	_fallRows = std::max(2, (int)floor(frame.size.height * 0.45));
	_fall.assign((size_t)_fallWidth * _fallRows * 4, 0);
	_fallHead = 0;
	_floorDb = NAN;
	_spanHz = 0;
	_centerHz = 0;
	_displayMode = kModeWFM;
	_clickedOffsetHz = 0.0f;
	_trackingArea = nil;
	_hovering = false;
	_tuneTarget = nil;
	_tuneAction = NULL;
	return self;
}

- (void)dealloc
{
	if (_trackingArea != nil) {
		[self removeTrackingArea:_trackingArea];
		[_trackingArea release];
	}
	[super dealloc];
}

- (void)updateTrackingAreas
{
	if (_trackingArea != nil) {
		[self removeTrackingArea:_trackingArea];
		[_trackingArea release];
	}
	_trackingArea = [[NSTrackingArea alloc] initWithRect:[self bounds]
		options:(NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved
			| NSTrackingActiveInKeyWindow) owner:self userInfo:nil];
	[self addTrackingArea:_trackingArea];
	[super updateTrackingAreas];
}

- (void)mouseEntered:(NSEvent*)event
{
	[self mouseMoved:event];
}

- (void)mouseExited:(NSEvent*)event
{
	_hovering = false;
	[self setNeedsDisplay:YES];
}

- (void)mouseMoved:(NSEvent*)event
{
	_hoverPoint = [self convertPoint:[event locationInWindow] fromView:nil];
	CGFloat waterfallTop = floor([self bounds].size.height * 0.45);
	_hovering = _hoverPoint.y >= 0.0 && _hoverPoint.y <= waterfallTop
		&& (_displayMode == kModeAM || _displayMode == kModeWFM
			|| _displayMode == kModeAir) && _spanHz > 0;
	[self setNeedsDisplay:YES];
}

- (void)setTuneTarget:(id)target action:(SEL)action
{
	_tuneTarget = target;
	_tuneAction = action;
}

- (float)clickedOffsetHz
{
	return _clickedOffsetHz;
}

- (void)mouseDown:(NSEvent*)event
{
	NSPoint point = [self convertPoint:[event locationInWindow] fromView:nil];
	NSRect bounds = [self bounds];
	CGFloat waterfallTop = floor(bounds.size.height * 0.45);
	if (point.y < 0.0 || point.y > waterfallTop || bounds.size.width <= 0.0
			|| _spanHz == 0 || _tuneTarget == nil || _tuneAction == NULL)
		return;
	CGFloat fraction = point.x / bounds.size.width;
	if (fraction < 0.0)
		fraction = 0.0;
	if (fraction > 1.0)
		fraction = 1.0;
	_clickedOffsetHz = (float)((fraction - 0.5) * (CGFloat)_spanHz);
	[NSApp sendAction:_tuneAction to:_tuneTarget from:self];
}

- (void)pushBins:(const std::vector<float>&)bins spanHz:(uint32)spanHz
	centerHz:(uint64)centerHz mode:(demod_mode)mode
{
	if (bins.empty())
		return;
	_spanHz = spanHz;
	_centerHz = centerHz;
	_displayMode = mode;
	// The FFT noise in each bin is intentionally lively.  Do not redraw that
	// randomness as full-scale movement of the green trace; retain enough
	// response for tuning while damping the frame-to-frame fizz.
	if (_bins.size() != bins.size()) {
		_bins = bins;
	} else {
		for (size_t i = 0; i < bins.size(); i++)
			_bins[i] += 0.18f * (bins[i] - _bins[i]);
	}

	// Scale against the measured noise floor rather than a fixed dB window.
	// A fixed window put the floor in the middle of the ramp, which is why
	// empty spectrum came out cyan instead of blue: where the floor lands
	// depends on gain, band and antenna, so it has to be measured.
	std::vector<float> sorted(bins);
	std::sort(sorted.begin(), sorted.end());
	float measuredFloor = sorted[sorted.size() / 2];
	if (!std::isfinite(_floorDb))
		_floorDb = measuredFloor;
	else
		_floorDb += 0.08f * (measuredFloor - _floorDb);

	uint8_t* row = &_fall[(size_t)_fallHead * _fallWidth * 4];
	for (int x = 0; x < _fallWidth; x++) {
		// Take the strongest bin covered by this column rather than a single
		// sample of it, so a narrow carrier cannot fall between columns when
		// there are more bins than pixels.
		size_t lo = (size_t)x * bins.size() / _fallWidth;
		size_t hi = (size_t)(x + 1) * bins.size() / _fallWidth;
		if (hi <= lo)
			hi = lo + 1;
		float db = bins[lo];
		for (size_t i = lo + 1; i < hi && i < bins.size(); i++) {
			if (bins[i] > db)
				db = bins[i];
		}
		float t = (db - _floorDb) / 45.0f;
		if (t < 0.0f) t = 0.0f;
		if (t > 1.0f) t = 1.0f;
		CGFloat r, g, b;
		WaterfallColour(t, r, g, b);
		row[x * 4 + 0] = (uint8_t)(r * 255.0f);
		row[x * 4 + 1] = (uint8_t)(g * 255.0f);
		row[x * 4 + 2] = (uint8_t)(b * 255.0f);
		row[x * 4 + 3] = 255;
	}
	_fallHead = (_fallHead + 1) % _fallRows;
	[self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
	NSRect b = [self bounds];
	[[NSColor colorWithCalibratedWhite:0.07 alpha:1.0] setFill];
	NSRectFill(b);
	if (_bins.empty())
		return;

	// An integral height gives every stored waterfall row exactly one display
	// point.  Fractional vertical resampling makes a scrolling raster crawl.
	CGFloat splitY = floor(b.size.height * 0.45);

	// Waterfall underneath, newest row at the top of its area.
	//
	// Drawn as one image. The previous version issued a filled rectangle per
	// cell - 512 x 120 of them every frame - which was both slow and why the
	// display looked coarse: it could not afford more columns than that.
	CGContextRef ctx = (CGContextRef)[[NSGraphicsContext currentContext]
		CGContext];
	{
		// Unroll the ring into a top-down image, newest row first.
		if ((int)_ordered.size() != _fallWidth * _fallRows * 4)
			_ordered.assign((size_t)_fallWidth * _fallRows * 4, 0);
		for (int r = 0; r < _fallRows; r++) {
			int src = (_fallHead - 1 - r + _fallRows * 2) % _fallRows;
			memcpy(&_ordered[(size_t)r * _fallWidth * 4],
				&_fall[(size_t)src * _fallWidth * 4],
				(size_t)_fallWidth * 4);
		}
		CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
		CGContextRef bmp = CGBitmapContextCreate(&_ordered[0], _fallWidth,
			_fallRows, 8, (size_t)_fallWidth * 4, cs,
			kCGImageAlphaPremultipliedLast);
		if (bmp != NULL) {
			CGImageRef img = CGBitmapContextCreateImage(bmp);
			if (img != NULL) {
				CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
				CGContextDrawImage(ctx,
					CGRectMake(0, 0, b.size.width, splitY), img);
				CGImageRelease(img);
			}
			CGContextRelease(bmp);
		}
		CGColorSpaceRelease(cs);
	}

	// Spectrum on top.
	NSBezierPath* path = [NSBezierPath bezierPath];
	CGFloat top = b.size.height;
	for (size_t i = 0; i < _bins.size(); i++) {
		float db = _bins[i];
		float t = (db - _floorDb) / 45.0f;
		if (t < 0.0f) t = 0.0f;
		if (t > 1.0f) t = 1.0f;
		CGFloat x = (CGFloat)i * b.size.width / (CGFloat)(_bins.size() - 1);
		CGFloat y = splitY + t * (top - splitY);
		if (i == 0)
			[path moveToPoint:NSMakePoint(x, y)];
		else
			[path lineToPoint:NSMakePoint(x, y)];
	}
	[[NSColor colorWithCalibratedRed:0.4 green:0.9 blue:0.5 alpha:1.0] setStroke];
	[path setLineWidth:1.0];
	[path stroke];

	// Centre marker.
	[[NSColor colorWithCalibratedWhite:1.0 alpha:0.25] setStroke];
	NSBezierPath* mid = [NSBezierPath bezierPath];
	[mid moveToPoint:NSMakePoint(b.size.width / 2, splitY)];
	[mid lineToPoint:NSMakePoint(b.size.width / 2, top)];
	[mid stroke];

	if (_hovering) {
		CGFloat fraction = _hoverPoint.x / b.size.width;
		if (fraction < 0.0) fraction = 0.0;
		if (fraction > 1.0) fraction = 1.0;
		double hz = (double)_centerHz
			+ (fraction - 0.5) * (double)_spanHz;
		NSString* value = _displayMode == kModeAM
			? [NSString stringWithFormat:@"%.3f kHz", hz / 1e3]
			: [NSString stringWithFormat:@"%.3f MHz", hz / 1e6];
		NSDictionary* attrs = @{
			NSFontAttributeName: [NSFont systemFontOfSize:11.0],
			NSForegroundColorAttributeName: [NSColor whiteColor]
		};
		NSSize textSize = [value sizeWithAttributes:attrs];
		NSRect tip = NSMakeRect(_hoverPoint.x + 12.0, _hoverPoint.y + 10.0,
			textSize.width + 10.0, textSize.height + 6.0);
		if (NSMaxX(tip) > NSMaxX(b))
			tip.origin.x = _hoverPoint.x - tip.size.width - 12.0;
		if (NSMaxY(tip) > splitY)
			tip.origin.y = _hoverPoint.y - tip.size.height - 10.0;
		[[NSColor colorWithCalibratedWhite:0.05 alpha:0.88] setFill];
		[[NSBezierPath bezierPathWithRoundedRect:tip xRadius:4.0 yRadius:4.0]
			fill];
		[value drawAtPoint:NSMakePoint(tip.origin.x + 5.0,
			tip.origin.y + 3.0) withAttributes:attrs];
	}
}
@end

// ------------------------------------------------------------- app delegate

@interface AppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation AppDelegate {
	NSWindow*		_window;
	SpectrumView*	_spectrum;
	VideoView*		_video;
	NSPopUpButton*	_stationMenu;
	NSTextField*	_freqField;
	NSTextField*	_freqLabel;
	NSPopUpButton*	_modeMenu;
	NSPopUpButton*	_presetMenu;
	NSPopUpButton*	_aspectMenu;
	NSTextField*	_aspectLabel;
	NSButton*		_stereoCheck;
	NSButton*		_startButton;
	NSMenuItem*		_dmbKeyItem;
	NSSlider*		_volume;
	NSSlider*		_squelch;
	NSTextField*	_status;
	NSTextField*	_meter;
	NSTimer*		_timer;
	BOOL			_stopping;
	BOOL			_probingStations;
	BOOL			_stationsLoading;
	BOOL			_devicePresent;
	CFAbsoluteTime	_lastDevicePoll;
	uint64			_probeFrequency;
	std::vector<int>	_displayedSubchannels;
	std::map<uint64, std::pair<std::vector<std::string>,
		std::vector<int> > > _stationCache;
}

- (void)applicationDidFinishLaunching:(NSNotification*)note
{
	_probingStations = NO;
	_stationsLoading = NO;
	_devicePresent = NO;
	_lastDevicePoll = 0;
	_probeFrequency = 0;
	NSMenu* mainMenu = [[NSMenu alloc] initWithTitle:@""];
	NSMenuItem* appItem = [[NSMenuItem alloc] initWithTitle:@"" action:nil
		keyEquivalent:@""];
	[mainMenu addItem:appItem];
	NSMenu* appMenu = [[NSMenu alloc] initWithTitle:@"R SDR"];
	NSMenuItem* keyItem = [[NSMenuItem alloc]
		initWithTitle:@"DMB Control Words..."
		action:@selector(showDmbKeys:) keyEquivalent:@","];
	[keyItem setKeyEquivalentModifierMask:
		NSEventModifierFlagCommand | NSEventModifierFlagOption];
	[keyItem setTarget:self];
	_dmbKeyItem = keyItem;
	[appMenu addItem:keyItem];
	[appMenu addItem:[NSMenuItem separatorItem]];
	[appMenu addItemWithTitle:@"Quit R SDR" action:@selector(terminate:)
		keyEquivalent:@"q"];
	[appItem setSubmenu:appMenu];
	[NSApp setMainMenu:mainMenu];
	// The old Air-mode help line occupied the bottom 32 points. Keep every
	// functional view at the same size and remove only that now-empty strip.
	const CGFloat bottomTrim = 32.0;
	NSRect frame = NSMakeRect(0, 0, 760, 520 - bottomTrim);
	_window = [[NSWindow alloc]
		initWithContentRect:frame
		styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
			| NSWindowStyleMaskMiniaturizable)
		backing:NSBackingStoreBuffered
		defer:NO];
	[_window setTitle:@"R SDR"];
	[_window center];

	NSView* content = [_window contentView];

	_spectrum = [[SpectrumView alloc]
		initWithFrame:NSMakeRect(12, 150 - bottomTrim, 736, 356)];
	[_spectrum setTuneTarget:self action:@selector(waterfallClicked:)];
	[content addSubview:_spectrum];

	// T-DMB video occupies the same area as the spectrum; only one of them is
	// ever visible.
	_video = [[VideoView alloc]
		initWithFrame:NSMakeRect(12, 150 - bottomTrim, 736, 356)];
	BOOL wideAspect = [[NSUserDefaults standardUserDefaults]
		objectForKey:@"DmbWideAspect"] == nil
		? YES : [[NSUserDefaults standardUserDefaults]
			boolForKey:@"DmbWideAspect"];
	[_video setWideAspect:wideAspect];
	[_video setHidden:YES];
	[content addSubview:_video];

	CGFloat y = 112 - bottomTrim;

	// Order: preset, mode, frequency. Air is single-frequency only, so it uses
	// the same editable MHz field rather than a one-item popup.
	_presetMenu = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(12, y - 4, 250, 26)];
	[_presetMenu addItemWithTitle:@"Presets..."];
	for (int i = 0; i < PresetCount(); i++) {
		const preset& p = PresetAt(i);
		[_presetMenu addItemWithTitle:[NSString stringWithUTF8String:p.label]];
	}
	[_presetMenu setTarget:self];
	[_presetMenu setAction:@selector(presetChosen:)];
	[content addSubview:_presetMenu];

	_modeMenu = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(272, y - 4, 100, 26)];
	for (int i = 0; i < kModeCount; i++) {
		[_modeMenu addItemWithTitle:
			[NSString stringWithUTF8String:ModeName((demod_mode)i)]];
	}
	[_modeMenu selectItemAtIndex:kModeWFM];
	[_modeMenu setTarget:self];
	[_modeMenu setAction:@selector(modeChanged:)];
	[content addSubview:_modeMenu];

	_freqLabel = [self label:@"MHz:" frame:NSMakeRect(382, y, 36, 20)];
	[content addSubview:_freqLabel];
	_freqField = [[NSTextField alloc] initWithFrame:NSMakeRect(420, y - 2, 96, 24)];
	[_freqField setStringValue:@"93.100"];
	[_freqField setTarget:self];
	[_freqField setAction:@selector(freqChanged:)];
	[content addSubview:_freqField];

	_stationMenu = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(524, y - 4, 130, 26)];
	[_stationMenu addItemWithTitle:@"Station..."];
	[_stationMenu setTarget:self];
	[_stationMenu setAction:@selector(stationChanged:)];
	[_stationMenu setHidden:YES];
	[content addSubview:_stationMenu];

	_startButton = [[NSButton alloc] initWithFrame:NSMakeRect(704, y - 7, 36, 32)];
	[_startButton setBezelStyle:NSBezelStyleCircular];
	[_startButton setTarget:self];
	[_startButton setAction:@selector(toggleRun:)];
	[content addSubview:_startButton];
	[self setTransportRunning:NO];

	y = 78 - bottomTrim;
	[content addSubview:[self label:@"Volume:" frame:NSMakeRect(12, y, 60, 20)]];
	_volume = [[NSSlider alloc] initWithFrame:NSMakeRect(76, y, 170, 20)];
	[_volume setMinValue:0.0];
	[_volume setMaxValue:1.0];
	[_volume setDoubleValue:0.7];
	[_volume setTarget:self];
	[_volume setAction:@selector(volumeChanged:)];
	[content addSubview:_volume];

	[content addSubview:[self label:@"Squelch:" frame:NSMakeRect(258, y, 60, 20)]];
	_squelch = [[NSSlider alloc] initWithFrame:NSMakeRect(322, y, 170, 20)];
	[_squelch setMinValue:-100.0];
	[_squelch setMaxValue:0.0];
	[_squelch setDoubleValue:-100.0];
	[_squelch setTarget:self];
	[_squelch setAction:@selector(squelchChanged:)];
	[content addSubview:_squelch];

	_stereoCheck = [[NSButton alloc] initWithFrame:NSMakeRect(500, y - 2, 100, 24)];
	[_stereoCheck setButtonType:NSButtonTypeSwitch];
	[_stereoCheck setTitle:@"Stereo"];
	BOOL fmStereo = [[NSUserDefaults standardUserDefaults]
		boolForKey:@"FmStereo"];
	[_stereoCheck setState:fmStereo ? NSControlStateValueOn
		: NSControlStateValueOff];
	[_stereoCheck setTarget:self];
	[_stereoCheck setAction:@selector(stereoChanged:)];
	[content addSubview:_stereoCheck];
	gReceiver.SetFmStereo(fmStereo);

	_aspectLabel = [self label:@"Aspect:" frame:NSMakeRect(500, y, 48, 20)];
	[_aspectLabel setHidden:YES];
	[content addSubview:_aspectLabel];
	_aspectMenu = [[NSPopUpButton alloc]
		initWithFrame:NSMakeRect(548, y - 4, 90, 26)];
	[_aspectMenu addItemWithTitle:@"16:9"];
	[_aspectMenu addItemWithTitle:@"4:3"];
	[_aspectMenu selectItemAtIndex:wideAspect ? 0 : 1];
	[_aspectMenu setTarget:self];
	[_aspectMenu setAction:@selector(aspectChanged:)];
	[_aspectMenu setHidden:YES];
	[content addSubview:_aspectMenu];

	_meter = [self label:@"" frame:NSMakeRect(644, y, 104, 20)];
	[content addSubview:_meter];

	_status = [self label:@"stopped"
		frame:NSMakeRect(12, 44 - bottomTrim, 736, 20)];
	[content addSubview:_status];

	[_window makeKeyAndOrderFront:nil];
	[NSApp activateIgnoringOtherApps:YES];

	_timer = [NSTimer scheduledTimerWithTimeInterval:0.05
		target:self selector:@selector(tick:) userInfo:nil repeats:YES];
	[self updateDeviceAvailability:YES];

	const char* even = getenv("RSDR_DMB_EVEN_CW");
	const char* odd = getenv("RSDR_DMB_ODD_CW");
	if ((even != NULL && even[0] != 0) || (odd != NULL && odd[0] != 0)) {
		std::string error;
		if (!gReceiver.SetDmbControlWords(even != NULL ? even : "",
				odd != NULL ? odd : "", error)) {
			[_status setStringValue:[NSString stringWithFormat:
				@"DMB key error: %s", error.c_str()]];
		}
	}
}

- (void)showDmbKeys:(id)sender
{
	NSAlert* alert = [[NSAlert alloc] init];
	[alert setMessageText:@"DMB DVB-CSA control words"];
	[alert setInformativeText:
		@"Enter control words you are authorized to use. Keys remain in memory "
		 "for this run and are not saved to disk. Leave both blank to disable."];
	[alert addButtonWithTitle:@"Apply"];
	[alert addButtonWithTitle:@"Cancel"];
	NSView* fields = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 390, 58)];
	NSTextField* evenLabel = [self label:@"Even:" frame:NSMakeRect(0, 34, 48, 20)];
	NSTextField* oddLabel = [self label:@"Odd:" frame:NSMakeRect(0, 4, 48, 20)];
	NSSecureTextField* evenField = [[NSSecureTextField alloc]
		initWithFrame:NSMakeRect(52, 31, 330, 24)];
	NSSecureTextField* oddField = [[NSSecureTextField alloc]
		initWithFrame:NSMakeRect(52, 1, 330, 24)];
	[evenField setPlaceholderString:@"12 or 16 hex digits"];
	[oddField setPlaceholderString:@"12 or 16 hex digits"];
	[fields addSubview:evenLabel];
	[fields addSubview:oddLabel];
	[fields addSubview:evenField];
	[fields addSubview:oddField];
	[alert setAccessoryView:fields];
	if ([alert runModal] != NSAlertFirstButtonReturn)
		return;
	std::string even([[evenField stringValue] UTF8String]);
	std::string odd([[oddField stringValue] UTF8String]);
	std::string error;
	if (!gReceiver.SetDmbControlWords(even, odd, error)) {
		NSAlert* failed = [[NSAlert alloc] init];
		[failed setMessageText:@"Could not apply DMB control word"];
		[failed setInformativeText:[NSString stringWithUTF8String:error.c_str()]];
		[failed runModal];
		return;
	}
	[_status setStringValue:even.empty() && odd.empty()
		? @"DMB descrambling disabled"
		: @"DMB control word loaded in memory"];
}

- (NSTextField*)label:(NSString*)text frame:(NSRect)frame
{
	NSTextField* f = [[NSTextField alloc] initWithFrame:frame];
	[f setStringValue:text];
	[f setBezeled:NO];
	[f setDrawsBackground:NO];
	[f setEditable:NO];
	[f setSelectable:NO];
	return f;
}

- (void)setTransportRunning:(BOOL)running
{
	NSString* symbol = running ? @"stop.fill" : @"play.fill";
	NSString* label = running ? @"Stop" : @"Play";
	NSImage* image = [NSImage imageWithSystemSymbolName:symbol
		accessibilityDescription:label];
	[_startButton setTitle:@""];
	[_startButton setImage:image];
	[_startButton setImagePosition:NSImageOnly];
	[_startButton setToolTip:label];
	[_startButton setAccessibilityLabel:label];
}

- (void)updateTransportVisibility
{
	BOOL dmb = [_modeMenu indexOfSelectedItem] == kModeDMB;
	BOOL stationSelected = !_stationsLoading
		&& [_stationMenu indexOfSelectedItem] > 0;
	// A DMB ensemble is not itself a playable programme.  Do not offer a
	// transport action until the user has explicitly selected a service.
	[_startButton setHidden:dmb && !stationSelected];
}

- (void)setReceiverControlsEnabled:(BOOL)enabled
{
	[_presetMenu setEnabled:enabled];
	[_modeMenu setEnabled:enabled];
	[_freqField setEnabled:enabled];
	[_stationMenu setEnabled:enabled && !_stationsLoading];
	[_aspectMenu setEnabled:enabled];
	[_stereoCheck setEnabled:enabled];
	[_volume setEnabled:enabled];
	[_squelch setEnabled:enabled];
	[_startButton setEnabled:enabled];
	[_dmbKeyItem setEnabled:enabled];
}

- (void)updateDeviceAvailability:(BOOL)force
{
	CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
	if (!force && now - _lastDevicePoll < 1.0)
		return;
	_lastDevicePoll = now;
	bool present = RtlDevice::DevicePresent();
	BOOL changed = present != (bool)_devicePresent;
	_devicePresent = present;
	if (changed || force)
		[self setReceiverControlsEnabled:present && !_stopping];
	if (!present) {
		[_status setStringValue:
			@"No RTL-SDR - waiting for connection..."];
		[_meter setStringValue:@"disconnected"];
		[self setTransportRunning:NO];
	} else if (_stopping) {
		[_status setStringValue:@"RTL-SDR reconnected - reinitializing..."];
		[_meter setStringValue:@"connected"];
	} else if (!gReceiver.IsRunning()) {
		[_status setStringValue:@"RTL-SDR connected - press Play"];
		[_meter setStringValue:@"connected"];
	}
}

- (void)toggleRun:(id)sender
{
	if (_stopping) {
		[_status setStringValue:@"finishing station scan - please wait"];
		return;
	}
	if (!_devicePresent)
		return;
	if (gReceiver.IsRunning()) {
		// Off the main thread: Stop() joins the capture thread, and if the
		// dongle has stopped answering that join can take seconds.
		_stopping = YES;
		[_startButton setEnabled:NO];
		[self setTransportRunning:NO];
		[_status setStringValue:@"stopping"];
		dispatch_async(dispatch_get_global_queue(
				DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
			gReceiver.Stop();
			dispatch_async(dispatch_get_main_queue(), ^{
				self->_stopping = NO;
				[self->_startButton setEnabled:YES];
				[self->_status setStringValue:@"stopped"];
			});
		});
		return;
	}
	if (gReceiver.Mode() == kModeDMB) {
		// Commit the menu selection again at the transport boundary. Depending on
		// AppKit event ordering, the menu action can be delivered while the FIC
		// receiver is still stopping; Start must not rely on that earlier action
		// having updated the worker-side subchannel already.
		NSInteger station = [_stationMenu indexOfSelectedItem] - 1;
		if (station < 0
				|| station >= (NSInteger)_displayedSubchannels.size()) {
			[_status setStringValue:@"select a Station before pressing Play"];
			return;
		}
		gReceiver.SetSubchannel(_displayedSubchannels[(size_t)station]);
	}
	[self applyFrequency];
	gReceiver.SetFicOnly(false);
	status_t err = gReceiver.Start();
	if (err != B_OK) {
		[_status setStringValue:err == B_DEV_NOT_READY
			? @"no RTL-SDR dongle found - plug one into this Mac"
			: @"could not open the RTL-SDR dongle"];
		return;
	}
	[self setTransportRunning:YES];
	[_status setStringValue:[NSString stringWithUTF8String:
		gReceiver.Description().c_str()]];
}

- (void)applyFrequency
{
	double value = [_freqField doubleValue];
	if (value <= 0.0)
		return;
	demod_mode mode = (demod_mode)[_modeMenu indexOfSelectedItem];
	double scale = mode == kModeAM ? 1e3 : 1e6;
	gReceiver.SetFrequency((uint64)(value * scale + 0.5));
}

- (void)resetStations
{
	_stationsLoading = gReceiver.Mode() == kModeDMB;
	[_stationMenu removeAllItems];
	[_stationMenu addItemWithTitle:_stationsLoading
		? @"Loading..." : @"Station..."];
	[_stationMenu selectItemAtIndex:0];
	[_stationMenu setEnabled:_devicePresent && !_stopping
		&& !_stationsLoading];
	_displayedSubchannels.clear();
	gReceiver.SetSubchannel(-1);
	[_video clearPicture];
	[self updateTransportVisibility];
}

- (void)aspectChanged:(id)sender
{
	BOOL wide = [_aspectMenu indexOfSelectedItem] == 0;
	[_video setWideAspect:wide];
	[[NSUserDefaults standardUserDefaults] setBool:wide forKey:@"DmbWideAspect"];
}

- (void)showStations:(const std::vector<std::string>&)labels
	subchannels:(const std::vector<int>&)subs
{
	_stationsLoading = NO;
	[_stationMenu removeAllItems];
	[_stationMenu addItemWithTitle:@"Station..."];
	for (size_t i = 0; i < labels.size(); i++) {
		[_stationMenu addItemWithTitle:
			[NSString stringWithUTF8String:labels[i].c_str()]];
	}
	_displayedSubchannels = subs;
	// A new ensemble never inherits the old programme selection, even if two
	// labels happen to match. The user explicitly chooses from the new FIC.
	[_stationMenu selectItemAtIndex:0];
	[_stationMenu setEnabled:_devicePresent && !_stopping];
	[self updateTransportVisibility];
}

- (void)probeStationsIfNeeded
{
	if (gReceiver.Mode() != kModeDMB || _stopping || !_devicePresent)
		return;
	uint64 hz = gReceiver.Frequency();
	if (_probingStations && gReceiver.IsRunning()) {
		// A second preset can be chosen while the first FIC read is active.
		// The receiver has already been retuned; associate the result with the
		// new frequency rather than caching it under the abandoned one.
		_probeFrequency = hz;
		[_status setStringValue:@"reading station list from FIC..."];
		return;
	}
	if (gReceiver.IsRunning() || _probingStations)
		return;
	std::map<uint64, std::pair<std::vector<std::string>,
		std::vector<int> > >::const_iterator cached = _stationCache.find(hz);
	if (cached != _stationCache.end()) {
		[self showStations:cached->second.first
			subchannels:cached->second.second];
		[_status setStringValue:@"stations loaded from FIC cache - press Start"];
		return;
	}

	_probeFrequency = hz;
	_probingStations = YES;
	gReceiver.SetFicOnly(true);
	[_startButton setEnabled:NO];
	[_status setStringValue:@"reading station list from FIC..."];
	status_t err = gReceiver.Start();
	if (err != B_OK) {
		_probingStations = NO;
		_stationsLoading = NO;
		gReceiver.SetFicOnly(false);
		[_stationMenu removeAllItems];
		[_stationMenu addItemWithTitle:@"Station..."];
		[_stationMenu setEnabled:_devicePresent && !_stopping];
		[_startButton setEnabled:YES];
		[_status setStringValue:err == B_DEV_NOT_READY
			? @"no RTL-SDR dongle found - plug one into this Mac"
			: @"could not read station list"];
	}
}

- (void)freqChanged:(id)sender
{
	[self applyFrequency];
	[self resetStations];
	[self probeStationsIfNeeded];
}

- (void)waterfallClicked:(id)sender
{
	demod_mode mode = (demod_mode)[_modeMenu indexOfSelectedItem];
	if (_stopping || (mode != kModeAM && mode != kModeWFM
			&& mode != kModeAir))
		return;
	double hz = (double)gReceiver.Frequency()
		+ (double)[_spectrum clickedOffsetHz];
	if (hz < 1.0)
		hz = 1.0;
	uint64 tunedHz = (uint64)(hz + 0.5);
	double scale = mode == kModeAM ? 1e3 : 1e6;
	[_freqField setStringValue:
		[NSString stringWithFormat:@"%.3f", (double)tunedHz / scale]];
	gReceiver.SetFrequency(tunedHz);
}

- (void)stationChanged:(id)sender
{
	NSInteger i = [_stationMenu indexOfSelectedItem] - 1;
	if (i >= 0 && i < (NSInteger)_displayedSubchannels.size()) {
		gReceiver.SetSubchannel(_displayedSubchannels[(size_t)i]);
		[_video clearPicture];
		[_status setStringValue:[NSString stringWithFormat:
			@"%@ selected - press Play", [_stationMenu titleOfSelectedItem]]];
	} else {
		gReceiver.SetSubchannel(-1);
		[_video clearPicture];
	}
	[self updateTransportVisibility];
}

- (void)modeChanged:(id)sender
{
	demod_mode mode = (demod_mode)[_modeMenu indexOfSelectedItem];
	BOOL wasAir = gReceiver.Mode() == kModeAir;
	[_stationMenu setHidden:mode != kModeDMB];
	[_aspectLabel setHidden:mode != kModeDMB];
	[_aspectMenu setHidden:mode != kModeDMB];
	[_stereoCheck setHidden:mode != kModeWFM];
	[_video setHidden:mode != kModeDMB];
	[_spectrum setHidden:mode == kModeDMB];
	[self updateTransportVisibility];
	gReceiver.SetMode(mode);
	if (mode == kModeAir) {
		gReceiver.SetCity(-1);
		if (!wasAir) {
			const uint64 kRkssApproachHz = 119100000ULL;
			gReceiver.SetFrequency(kRkssApproachHz);
		}
	} else if (wasAir) {
		gReceiver.SetCity(-1);
	}
	// Broadcast AM is conventionally tuned in kHz; the other modes use MHz.
	// The receiver still stores Hz, so changing decoder modes cannot silently
	// retune by a factor of one thousand.
	uint64 hz = gReceiver.Frequency();
	if (mode == kModeAM) {
		[_freqLabel setStringValue:@"kHz:"];
		[_freqField setStringValue:
			[NSString stringWithFormat:@"%.3f", hz / 1e3]];
	} else {
		[_freqLabel setStringValue:@"MHz:"];
		[_freqField setStringValue:
			[NSString stringWithFormat:@"%.3f", hz / 1e6]];
	}
	// Start Air at the deliberately permissive minimum. In city-monitor mode
	// -100 maps to 3 dB over the measured noise floor; the user can raise it
	// afterwards if that keeps noise open too often.
	if (mode == kModeAir) {
		[_squelch setDoubleValue:-100.0];
		[self squelchChanged:nil];
	}
}

- (void)applyPresetAtIndex:(NSInteger)i probeStations:(BOOL)probe
{
	if (i < 0 || i >= PresetCount())
		return;
	const preset& p = PresetAt((int)i);
	[_modeMenu selectItemAtIndex:p.mode];
	[self modeChanged:nil];
	double shown = p.mode == kModeAM
		? p.frequencyHz / 1e3 : p.frequencyHz / 1e6;
	[_freqField setStringValue:
		[NSString stringWithFormat:@"%.3f", shown]];
	[self resetStations];
	[self applyFrequency];
	if (probe)
		[self probeStationsIfNeeded];
}

- (void)presetChosen:(id)sender
{
	NSInteger i = [_presetMenu indexOfSelectedItem] - 1;
	if (i < 0 || i >= PresetCount() || _stopping)
		return;

	if (!gReceiver.IsRunning()) {
		[self applyPresetAtIndex:i probeStations:YES];
		return;
	}

	demod_mode targetMode = PresetAt((int)i).mode;

	// librtlsdr cannot safely mix a control transfer with the async USB run
	// that is being cancelled.  A live preset used to queue gain/frequency
	// writes immediately; libusb_submit_transfer then occasionally dereferenced
	// a transfer which its hotplug/completion thread had just removed.  Finish
	// the old stream first, apply the new preset while the device is idle, and
	// restart analog playback after applying it. DMB remains stopped because a
	// station still has to be selected. This also gives the DAB/FIC/MSC state
	// one clean epoch.
	_stopping = YES;
	[self setReceiverControlsEnabled:NO];
	[self setTransportRunning:NO];
	[_status setStringValue:@"switching preset..."];
	dispatch_async(dispatch_get_global_queue(
			DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
		gReceiver.Stop();
		dispatch_async(dispatch_get_main_queue(), ^{
			self->_stopping = NO;
			// If the old preset was only doing its one-shot FIC scan, Stop()
			// ended that scan before it could clear this flag. Leaving it set
			// makes probeStationsIfNeeded reject the new preset forever and the
			// Station control remains stuck at Loading....
			self->_probingStations = NO;
			gReceiver.SetFicOnly(false);
			[self setReceiverControlsEnabled:self->_devicePresent];
			if (!self->_devicePresent) {
				self->_lastDevicePoll = 0;
				[self updateDeviceAvailability:YES];
				return;
			}
			[self applyPresetAtIndex:i probeStations:YES];
			if (targetMode != kModeDMB) {
				status_t err = gReceiver.Start();
				[self setTransportRunning:err == B_OK];
				[self->_status setStringValue:err == B_OK
					? @"preset ready" : @"could not restart receiver"];
			}
		});
	});
}

- (void)volumeChanged:(id)sender
{
	gReceiver.SetVolume((float)[_volume doubleValue]);
}

- (void)squelchChanged:(id)sender
{
	double db = [_squelch doubleValue];
	gReceiver.SetSquelchDb(db <= -100.0 ? -200.0f : (float)db);
}

- (void)stereoChanged:(id)sender
{
	BOOL enabled = [_stereoCheck state] == NSControlStateValueOn;
	gReceiver.SetFmStereo(enabled);
	[[NSUserDefaults standardUserDefaults] setBool:enabled forKey:@"FmStereo"];
}

- (void)tick:(NSTimer*)timer
{
	if (!_devicePresent || !gReceiver.IsRunning())
		[self updateDeviceAvailability:NO];
	if (!gReceiver.IsRunning())
		return;

	// The dongle can be pulled out mid-read. The capture thread gives up
	// rather than retrying forever, so all that is needed here is to notice,
	// tear down off the main thread - Stop() joins threads and must never run
	// on the UI thread - and invite the user to plug it back in.
	if (gReceiver.DeviceLost() && !_stopping) {
		_devicePresent = NO;
		_stopping = YES;
		[self setReceiverControlsEnabled:NO];
		[self setTransportRunning:NO];
		[_status setStringValue:
			@"USB connection lost - waiting for reconnection..."];
		[_video clearPicture];
		dispatch_async(dispatch_get_global_queue(
				DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
			gReceiver.Stop();
			gReceiver.CloseLostDevice();
			dispatch_async(dispatch_get_main_queue(), ^{
				self->_stopping = NO;
				self->_lastDevicePoll = 0;
				[self updateDeviceAvailability:YES];
			});
		});
		return;
	}
	if (_stopping)
		return;
	if (gReceiver.Mode() == kModeDMB) {
		std::vector<uint8> config;
		std::vector<DmbAudio::video_unit> units;
		gReceiver.TakeVideo(config, units);
		if (!config.empty())
			[_video setConfig:config];
		uint64 audioPts = 0;
		bool haveAudioPts = gReceiver.AudioPlaybackPts(audioPts);
		[_video setAudioPts:audioPts valid:haveAudioPts];
		for (size_t i = 0; i < units.size(); i++)
			[_video pushUnit:units[i].data pts:units[i].pts90k
				hasPts:units[i].hasPts];

		std::vector<std::string> labels;
		std::vector<int> subs;
		gReceiver.Services(labels, subs);
		bool stationsChanged = labels.size() != _displayedSubchannels.size();
		if (!stationsChanged) {
			for (size_t i = 0; i < labels.size(); i++) {
				NSString* shown = [[_stationMenu itemAtIndex:(NSInteger)i + 1]
					title];
				if (![shown isEqualToString:
						[NSString stringWithUTF8String:labels[i].c_str()]]
						|| _displayedSubchannels[i] != subs[i]) {
					stationsChanged = true;
					break;
				}
			}
		}
		// A stopped T-DMB preset performs only this one acquisition. Once FIC
		// has supplied a service list, cache it by ensemble frequency and give
		// the dongle back. Do not expose a selectable Station until Stop has
		// completed: the old order showed Station first while Play was still
		// disabled, so a valid mYTN selection could appear ready but its click on
		// Play was silently ignored during the stopping interval.
		if (_probingStations && !labels.empty() && !_stopping) {
			_stationCache[_probeFrequency] = std::make_pair(labels, subs);
			_probingStations = NO;
			_stopping = YES;
			[_stationMenu setEnabled:NO];
			[_startButton setEnabled:NO];
			[_status setStringValue:@"station list found - preparing playback..."];
			dispatch_async(dispatch_get_global_queue(
					DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
				gReceiver.Stop();
				dispatch_async(dispatch_get_main_queue(), ^{
					self->_stopping = NO;
					gReceiver.SetFicOnly(false);
					[self showStations:labels subchannels:subs];
					[self->_startButton setEnabled:YES];
					[self setTransportRunning:NO];
					[self->_status setStringValue:
						@"select a Station, then press Play"];
				});
			});
			return;
		}

		if (stationsChanged && !labels.empty())
			[self showStations:labels subchannels:subs];

		uint64 rsOk = 0, rsBlocks = 0, audioUnits = 0, fibsOk = 0, fibsTried = 0;
		int sub = -1, dabFrames = 0;
		int blockKb = 0;
		int gain = 0;
		uint32 rate = 0;
		float nullDepth = 1.0f, rfDb = -120.0f;
		gReceiver.DabStats(rsOk, rsBlocks, audioUnits, sub, fibsOk, fibsTried,
			dabFrames, blockKb, rate, nullDepth, rfDb, gain);
		bool cwEnabled = false;
		uint64 cwValid = 0, cwTried = 0;
		gReceiver.DescramblerStats(cwEnabled, cwValid, cwTried);
		NSString* cwStatus = cwEnabled
			? [NSString stringWithFormat:@"  CW %llu/%llu PES",
				(unsigned long long)cwValid, (unsigned long long)cwTried]
			: @"";
		// FIB counts first: an empty station list with zero FIBs means the
		// signal is not being decoded at all, which is a different problem
		// from a multiplex that carries no TS services.
		[_status setStringValue:[NSString stringWithFormat:
			@"T-DMB %.3f MS/s  gain %.1f dB  RF %.0f dB  null %.3f (needs <0.35)  "
			"frames %d  FIB %llu/%llu  subch %d  RS %llu/%llu  AU %llu  vid %d%@",
			rate / 1e6, gain / 10.0, rfDb, nullDepth, dabFrames,
			(unsigned long long)fibsOk, (unsigned long long)fibsTried, sub,
			(unsigned long long)rsOk, (unsigned long long)rsBlocks,
			(unsigned long long)audioUnits, [_video framesShown], cwStatus]];
		return;
	}

	Snapshot snap;
	gReceiver.Fetch(snap);
	[_spectrum pushBins:snap.spectrum spanHz:snap.spectrumRate
		centerHz:gReceiver.Frequency() mode:gReceiver.Mode()];
	[_meter setStringValue:[NSString stringWithFormat:
		@"peak %.2f  %@", snap.peak, snap.squelchOpen ? @"open" : @"muted"]];

	if (gReceiver.CityIndex() >= 0) {
		std::string live = gReceiver.ActiveChannelLabel();
		std::string best;
		float over = 0.0f;
		gReceiver.BestChannel(best, over);
		if (!live.empty()) {
			[_status setStringValue:[NSString stringWithFormat:@"%s",
				live.c_str()]];
		} else if (!best.empty()) {
			// Show the strongest thing in the window and how far short of the
			// threshold it is, so a silent band can be told apart from a
			// squelch set too high.
			[_status setStringValue:[NSString stringWithFormat:
				@"%s - quiet.  strongest: %s at +%.1f dB over noise",
				AirCityAt(gReceiver.CityIndex()).name, best.c_str(), over]];
		} else {
			[_status setStringValue:[NSString stringWithFormat:
				@"%s - listening", AirCityAt(gReceiver.CityIndex()).name]];
		}
		return;
	}
	[_status setStringValue:[NSString stringWithFormat:
		@"%s   %.0f kS/s   underruns %llu   dropped blocks %llu   offset %+.0f Hz",
		gReceiver.Description().c_str(), snap.iqRate / 1000.0,
		(unsigned long long)snap.underruns,
		(unsigned long long)snap.dropped, snap.carrierOffset]];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)a
{
	return YES;
}

- (void)applicationWillTerminate:(NSNotification*)note
{
	gReceiver.Stop();
}
@end

int
main(int argc, const char** argv)
{
	@autoreleasepool {
		NSApplication* app = [NSApplication sharedApplication];
		[app setActivationPolicy:NSApplicationActivationPolicyRegular];
		AppDelegate* delegate = [[AppDelegate alloc] init];
		[app setDelegate:delegate];
		[app run];
	}
	return 0;
}
