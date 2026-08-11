#include "Receiver.h"

#include <Autolock.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

// C++11 requires one out-of-class definition when the constant is passed by
// reference to std::vector::push_back().
const int Receiver::kDefaultGainTenths;

static std::vector<int>
DabProbeGains(const std::vector<int>& supported)
{
	std::vector<int> gains;
	if (supported.empty()) {
		gains.push_back(Receiver::kDefaultGainTenths);
		return gains;
	}

	// Sample seven evenly spaced points from the tuner-reported range.  The
	// list therefore adapts to other tuner models and contains no knowledge of
	// a Korean ensemble, frequency, or service.  Always use exact supported
	// values: librtlsdr does not promise to round an arbitrary manual gain.
	const size_t count = std::min((size_t)7, supported.size());
	for (size_t i = 0; i < count; i++) {
		size_t index = count == 1 ? 0
			: (i * (supported.size() - 1) + (count - 1) / 2) / (count - 1);
		int gain = supported[index];
		if (gains.empty() || gains.back() != gain)
			gains.push_back(gain);
	}
	return gains;
}

Receiver::Receiver()
	:
	fDabSubchannel(-1),
	fRequestedDabSubchannel(-1),
	fDabQuality(-1.0f),
	fThread(-1),
	fStopRequested(false),
	fLock("receiver"),
	fMode(kModeWFM),
	fFrequency(92500000ULL),
	fFineTune(0.0f),
	fGain(kDefaultGainTenths),
	// The Haiku R820T2 was measured 84 ppm slow on the connected dongle.
	// Apply the correction by default; macOS keeps its independent 0 ppm
	// default because the two devices have different crystals.
	fPpm(-84),
	fSquelchDb(-200.0f),
	fFmStereo(false),
	fModeDirty(true),
	fSpectrumRate(0),
	fSpectrumSequence(0),
	fSignalDb(-120.0f),
	fCarrierOffset(0.0f),
	fLastDigitalAudio(0),
	fDigitalAudioDebt(0.0),
	fSquelchOpen(true),
	fScanning(false),
	fScanParked(false),
	fScanCentreHz(0),
	fScanActiveAt(0),
	fScanSteppedAt(0),
	fDspLoad(0.0f),
	fAudioSamples(0),
	fAppliedGain(-1),
	fLastSpectrum(0),
	fDmbAudioOnly(false),
	fDmbVideoSnapshotTaken(false)
{
	fWaveform.assign(kWaveformSamples, 0.0f);
}

Receiver::~Receiver()
{
	Stop();
}

std::string
Receiver::DeviceDescription() const
{
	if (!fDevice.IsOpen())
		return "no device";
	return fDevice.DeviceName() + " / " + fDevice.TunerName();
}

status_t
Receiver::Start()
{
	if (fThread >= 0)
		return B_OK;

	status_t err = fDevice.Open(0);
	if (err != B_OK) {
		BAutolock lock(fLock);
		fStatus = err == B_DEV_NOT_READY
			? "no RTL-SDR device found"
			: "could not open RTL-SDR device";
		return err;
	}

	{
		BAutolock lock(fLock);
		// Force a full reconfigure on the first block.
		fModeDirty = true;
		fStatus = "starting";
	}

	mode_plan plan = PlanForMode(fMode);
	fDevice.RequestSampleRate(plan.tunerSampleRate);
	fDevice.RequestPpmCorrection(fPpm);
	fDevice.RequestGain(fGain);
	fDevice.RequestFrequency((uint32)fFrequency);
	fAppliedGain = fGain;

	err = fDevice.Start();
	if (err != B_OK) {
		BAutolock lock(fLock);
		fStatus = "could not start capture thread";
		return err;
	}

	fStopRequested = false;
	// Keep audio production ahead of the UI repaint thread on the target's
	// single-core Atom. Capture sleeps inside read_sync most of the time, so
	// sharing its urgent-display class does not starve USB input.
	// Below the capture thread on purpose. Raising this to the capture
	// thread's priority is what caused the 13% sample loss described there:
	// on one core, two equal-priority threads round-robin, and the DSP holds
	// the CPU for a whole quantum while the dongle's FIFO overflows.
	fThread = spawn_thread(&Receiver::_ThreadEntry, "rsdr demod",
		B_DISPLAY_PRIORITY, this);
	if (fThread < 0) {
		BAutolock lock(fLock);
		fStatus = "could not start demodulator thread";
		return fThread;
	}
	resume_thread(fThread);
	return B_OK;
}

void
Receiver::Stop()
{
	if (fThread >= 0) {
		fStopRequested = true;
		status_t result;
		wait_for_thread(fThread, &result);
		fThread = -1;
	}
	fDevice.Stop();
	fAudio.Shutdown();
	fDevice.Close();

	BAutolock lock(fLock);
	fStatus = "stopped";
}

void
Receiver::SetMode(demod_mode mode)
{
	uint32 wantRate = 0;
	{
		BAutolock lock(fLock);
		if (mode == fMode)
			return;
		fMode = mode;
		fModeDirty = true;
		wantRate = PlanForMode(mode).tunerSampleRate;
	}

	// The capture thread applies this between two reads and bumps the
	// generation, which is what makes the demodulator rebuild its filters.
	fDevice.RequestSampleRate(wantRate);
}

demod_mode
Receiver::Mode() const
{
	BAutolock lock(fLock);
	return fMode;
}

void
Receiver::SetFrequency(uint64 hz)
{
	{
		BAutolock lock(fLock);
		fFrequency = hz;
		if (fMode == kModeDMB) {
			fModeDirty = true;
			// Publish an empty service list at once so the UI shows the new
			// ensemble's stations, not the previous preset's. The demod thread
			// only clears these on its next block (fModeDirty), which leaves a
			// window where Fetch() would repopulate the menu with stale names.
			fDabEnsemble.clear();
			fDabServices.clear();
			fDabServiceChoices.clear();
			fDabResult = DabProbe::result();
			fDabQuality = -1.0f;
		}
	}
	fDevice.RequestFrequency((uint32)hz);
}

uint64
Receiver::Frequency() const
{
	BAutolock lock(fLock);
	return fFrequency;
}

void
Receiver::SetFineTune(float hz)
{
	BAutolock lock(fLock);
	fFineTune = hz;
	fDemod.SetFineTune(hz);
}

float
Receiver::FineTune() const
{
	BAutolock lock(fLock);
	return fFineTune;
}

void
Receiver::SetGain(int tenthsDb)
{
	{
		BAutolock lock(fLock);
		fGain = tenthsDb;
		fAppliedGain = tenthsDb;
	}
	fDevice.RequestGain(tenthsDb);
}

int
Receiver::Gain() const
{
	BAutolock lock(fLock);
	return fGain;
}

void
Receiver::SetSquelchDb(float db)
{
	BAutolock lock(fLock);
	fSquelchDb = db;
	fDemod.SetSquelch(db);
}

void
Receiver::SetFmStereo(bool enabled)
{
	BAutolock lock(fLock);
	if (fFmStereo == enabled)
		return;
	fFmStereo = enabled;
	// Reconfigure the demodulator and audio ring together on the DSP thread;
	// changing one without the other would interpret interleaved stereo as a
	// half-rate mono stream for one block.
	fModeDirty = true;
}

void
Receiver::SetVolume(float gain)
{
	fAudio.SetVolume(gain);
}

void
Receiver::SetPpm(int ppm)
{
	{
		BAutolock lock(fLock);
		fPpm = ppm;
	}
	fDevice.RequestPpmCorrection(ppm);
}

int
Receiver::Ppm() const
{
	BAutolock lock(fLock);
	return fPpm;
}

void
Receiver::SetDabSubchannel(int subchannel)
{
	BAutolock lock(fLock);
	fRequestedDabSubchannel = subchannel;
}

void
Receiver::TakeDmbVideo(std::vector<uint8>& config,
	std::vector<DmbAudio::video_unit>& units)
{
	BAutolock lock(fLock);
	config = fDmbVideoConfig;
	units.swap(fDmbVideoUnits);
}

void
Receiver::_CollectDmbVideo()
{
	std::vector<DmbAudio::video_unit> units;
	DmbAudio::video_unit unit;
	while (fDmbAudio.TakeVideoUnit(unit))
		units.push_back(unit);
	if (units.empty() && fDmbAudio.VideoConfig().empty())
		return;

	BAutolock lock(fLock);
	fDmbVideoConfig = fDmbAudio.VideoConfig();
	// If a DMB block takes longer than its real-time interval, the Atom is
	// overloaded. Keep one access unit as a best-effort still image, then
	// discard video so the CPU is reserved for MSC recovery and BSAC audio.
	if (fDspLoad > 1.0f)
		fDmbAudioOnly = true;
	if (!fDmbAudioOnly) {
		fDmbVideoUnits.insert(fDmbVideoUnits.end(), units.begin(), units.end());
	} else if (!fDmbVideoSnapshotTaken && !units.empty()) {
		fDmbVideoUnits.clear();
		fDmbVideoUnits.push_back(units.front());
		fDmbVideoSnapshotTaken = true;
	}
}

status_t
Receiver::_ThreadEntry(void* cookie)
{
	static_cast<Receiver*>(cookie)->_DemodLoop();
	return B_OK;
}

void
Receiver::_DemodLoop()
{
	SdrDevice::Block block;
	std::vector<float> audio;
	std::vector<dsp::Cf> dmbIq;
	std::vector<float> dmbSoft;
	uint32 configuredGeneration = 0;
	bool configured = false;
	bool dmbPreviousBlock = false;
	uint64 dmbFrameCounter = 0;
	std::vector<int> dabGainCandidates;
	size_t dabGainIndex = 0;
	bool dabGainSweepActive = false;
	bool dabGainSweepJustFinished = false;
	double dabGainBestScore = -1e30;
	int dabGainBest = kDefaultGainTenths;
	DabFic dabGainBestFic;
	DabProbe::result dabGainBestResult;

	// One line a second to /tmp/rsdr-stats.log. The GUI's own status line
	// cannot be read back over ssh, and problems that only appear with the
	// window open (redraw starving this thread, for one) are otherwise
	// invisible from outside.
	FILE* statsLog = fopen("/tmp/rsdr-stats.log", "w");
	bigtime_t lastStats = system_time();
	uint64 statsIq = 0;
	uint64 statsAudio = 0;
	uint64 statsUnder = 0;

	// Exponentially smoothed, so a single scheduling hiccup does not make the
	// load readout jump to 100%.
	float loadAverage = 0.0f;

	while (!fStopRequested) {
		status_t err = fDevice.NextBlock(block, 250000);
		if (err != B_OK) {
			uint32 consecutive = fDevice.ConsecutiveReadErrors();
			if (consecutive > 0) {
				BAutolock lock(fLock);
				fStatus = "USB read stopped - reconnect the RTL-SDR";
			}
			continue;
		}

		uint32 deviceGeneration = fDevice.Generation();
		if (block.generation != deviceGeneration) {
			// Captured with the previous sample rate.
			continue;
		}

		demod_mode mode;
		bool modeDirty;
		bool fmStereo;
		{
			BAutolock lock(fLock);
			mode = fMode;
			modeDirty = fModeDirty;
			fmStereo = fFmStereo;
			fModeDirty = false;
		}

		if (statsLog != NULL && system_time() - lastStats >= 1000000) {
			bigtime_t now = system_time();
			double dt = (double)(now - lastStats) / 1e6;
			uint64 iq = fDevice.BytesRead();
			uint64 under = fAudio.Underruns();
			fprintf(statsLog, "%-4s iq %8.0f/s audio %6.0f/s under %5.0f/s "
				"drop %llu readerr %llu gen %u/%u rate %u block %luK cfg %d "
				"load %3.0f%% rf %6.1f",
				ModeName(mode),
				(double)(iq - statsIq) / 2.0 / dt,
				(double)(fAudioSamples - statsAudio) / dt,
				(double)(under - statsUnder) / dt,
				(unsigned long long)fDevice.DroppedBlocks(),
				(unsigned long long)fDevice.ReadErrors(),
				(unsigned)block.generation, (unsigned)fDevice.Generation(),
				(unsigned)fDevice.SampleRate(),
				(unsigned long)(fDevice.BlockBytes() / 1024),
				configured ? 1 : 0,
				loadAverage * 100.0, fSignalDb);
			if (mode == kModeDMB) {
				const DabMsc::stats& msc = fDabMsc.Stats();
				const DmbAudio::stats& av = fDmbAudio.Stats();
				fprintf(statsLog, " sub %d rs %llu/%llu au %llu",
					fDabSubchannel,
					(unsigned long long)msc.rsOk,
					(unsigned long long)msc.blocksTried,
					(unsigned long long)av.decodedUnits);
			}
			fputc('\n', statsLog);
			fflush(statsLog);
			statsIq = iq;
			statsAudio = fAudioSamples;
			statsUnder = under;
			lastStats = now;
		}

		if (!configured || modeDirty || configuredGeneration != deviceGeneration) {
			uint32 actualRate = fDevice.SampleRate();
			if (actualRate == 0)
				continue;

			mode_plan plan = PlanForMode(mode);
			if (actualRate != plan.tunerSampleRate) {
				// A mode whose rate does not match what the device is
				// currently running at; SetMode restarts the stream for that,
				// so wait for a block captured at the new rate.
				continue;
			}

			fDemod.Configure(mode, actualRate);
			bool useStereo = mode == kModeWFM && fmStereo;
			{
				BAutolock lock(fLock);
				fDemod.SetFineTune(fFineTune);
				fDemod.SetSquelch(fSquelchDb);
				fDemod.SetFmStereo(useStereo);
			}
			// T-DMB decodes in bursty per-read chunks and the signal fades in
			// and out; a deep audio ring rides over brief bad reads at the cost
			// of a few seconds of latency (fine for broadcast). The analogue
			// modes stay at the default 1 s for responsive tuning.
			fAudio.SetRingSeconds(mode == kModeDMB ? 6 : 1);
			fAudio.Configure(plan.audioSampleRate, useStereo ? 2 : 1);
			if (mode == kModeDMB) {
				fDabProbe.Reset();
				fDabFic.Reset();
				fDabMsc = DabMsc();
				fDmbAudio.Reset();
				fDabSubchannel = -1;
				BAutolock lock2(fLock);
				fDabEnsemble.clear();
				fDabServices.clear();
				fDabServiceChoices.clear();
				fDabQuality = -1.0f;
				fDmbVideoConfig.clear();
				fDmbVideoUnits.clear();
				// Haiku's Atom cannot sustain H.264 decode alongside the DAB
				// front-end. Start in audio-first mode and retain only one still
				// access unit; this also prevents a long-running video queue from
				// growing or exercising the fragile decoder repeatedly.
				fDmbAudioOnly = true;
				fDmbVideoSnapshotTaken = false;
				dmbPreviousBlock = false;
				dmbFrameCounter = 0;
				dabGainCandidates = DabProbeGains(fDevice.GainSteps());
				dabGainIndex = 0;
				dabGainSweepActive = !dabGainCandidates.empty();
				dabGainSweepJustFinished = false;
				dabGainBestScore = -1e30;
				dabGainBest = dabGainCandidates.empty()
					? kDefaultGainTenths : dabGainCandidates[0];
				dabGainBestFic.Reset();
				dabGainBestResult = DabProbe::result();
				if (dabGainSweepActive) {
					fDevice.RequestGain(dabGainCandidates[0]);
					fAppliedGain = dabGainCandidates[0];
				}
			}
			fLastDigitalAudio = 0;
			fDigitalAudioDebt = 0.0;
			configuredGeneration = deviceGeneration;
			configured = true;

			BAutolock lock(fLock);
			if (mode == kModeDMB) {
				char buf[160];
				snprintf(buf, sizeof(buf), "T-DMB: tuner %.3f MS/s, %d KB per "
					"read, starting automatic gain search", actualRate / 1e6,
					(int)(fDevice.BlockBytes() / 1024));
				fStatus = buf;
			} else {
				char buf[128];
				snprintf(buf, sizeof(buf), "%s, tuner %.4f MS/s, audio %u Hz",
					ModeName(mode), actualRate / 1e6,
					(unsigned)plan.audioSampleRate);
				fStatus = buf;
			}
			// This block was captured before the first sweep gain request was
			// applied. SdrDevice drains the queue at the control transfer; skip the
			// block already in our hands as well.
			if (mode == kModeDMB && dabGainSweepActive)
				continue;
		}

		if (block.data.empty())
			continue;

		if (ModeIsDigital(mode)) {
			bigtime_t started = system_time();
			float digitalSignalDb = -120.0f;
			{
				// DMB used to run the entire analogue Demodulator merely to obtain
				// this meter value (9.6% of the live profile). Sparse raw-IQ power
				// sampling gives the same UI telemetry for negligible cost.
				double power = 0.0;
				size_t measured = 0;
				for (size_t j = 0; j + 1 < block.data.size(); j += 128) {
					float re = ((float)block.data[j] - 127.4f) * (1.0f / 128.0f);
					float im = ((float)block.data[j + 1] - 127.4f)
						* (1.0f / 128.0f);
					power += re * re + im * im;
					measured++;
				}
				if (measured > 0)
					digitalSignalDb = 10.0f * log10f((float)(power / measured)
						+ 1e-14f);
			}

			size_t produced = 0;
			int tracked = 0;
			float playbackNullDepth = 1.0f;
			float playbackFineHz = 0.0f;
			int requestedSubchannel;
			{
				BAutolock requestLock(fLock);
				requestedSubchannel = fRequestedDabSubchannel;
			}
			if (mode == kModeDMB) {
				if (requestedSubchannel < 0 && dabGainSweepActive) {
					fDabProbe.Feed(&block.data[0], block.data.size());
					const DabProbe::result result = fDabProbe.Result();
					const DabFic& fic = fDabProbe.Fic();
					double fibRate = result.fibsTried > 0
						? (double)result.fibsOk / (double)result.fibsTried : 0.0;
					double score = fibRate * 1000.0
						- (double)result.nullDepth * 10.0
						+ (double)fic.Services().size() * 0.01;
					if (score > dabGainBestScore) {
						dabGainBestScore = score;
						dabGainBest = dabGainCandidates[dabGainIndex];
						dabGainBestFic = fic;
						dabGainBestResult = result;
					}

					{
						BAutolock sweepLock(fLock);
						fSignalDb = digitalSignalDb;
						fDabResult = result;
						char status[192];
						snprintf(status, sizeof(status), "T-DMB auto gain %lu/%lu: "
							"%.1f dB, FIB %u/%u", (unsigned long)dabGainIndex + 1,
							(unsigned long)dabGainCandidates.size(),
							dabGainCandidates[dabGainIndex] / 10.0,
							(unsigned)result.fibsOk, (unsigned)result.fibsTried);
						fStatus = status;
					}

					if (++dabGainIndex < dabGainCandidates.size()) {
						fDabProbe.Reset();
						fDevice.RequestGain(dabGainCandidates[dabGainIndex]);
						fAppliedGain = dabGainCandidates[dabGainIndex];
						continue;
					}

					// Publish only the winning FIC map.  Until this point Station stays
					// empty, so the UI cannot promote a half-finished candidate to
					// programme playback.
					fDabFic = dabGainBestFic;
					fDevice.RequestGain(dabGainBest);
					fAppliedGain = dabGainBest;
					dabGainSweepActive = false;
					dabGainSweepJustFinished = true;
				} else if (requestedSubchannel < 0) {
					// The Haiku UI deliberately keeps capture alive after discovery so
					// Play can promote the same safe synchronous stream. Do no OFDM/MSC
					// work while it is waiting for a Station selection.
					continue;
				}
				// Each USB read is decoded on its own. Carrying the untracked tail
				// across reads (as macOS _ProcessDab does) was measured to HURT here:
				// Haiku's inter-read USB gap is large enough that prepended stale
				// samples broke null/frame locking (12 nolock vs 3) and stalled the RS
				// byte stream. Fresh per read plus the self-healing RS re-sync in
				// DabMsc gives reliable lock and a flowing byte stream instead.
				dmbIq.resize(block.data.size() / 2);
				std::vector<dsp::Cf>& iq = dmbIq;
				{
					// One 6 MB block of unsigned bytes becomes 3 M floats every
					// 1.5 s; at 2.048 MS/s this scalar convert was measured near
					// 7% of the DAB thread. re and im are just the interleaved
					// bytes, so scale the whole buffer flat.
					const uint8* src = &block.data[0];
					float* dst = reinterpret_cast<float*>(&iq[0]);
					size_t n = block.data.size();
					size_t j = 0;
#if defined(__SSE2__)
					const __m128 bias = _mm_set1_ps(127.4f);
					const __m128 scale = _mm_set1_ps(1.0f / 128.0f);
					const __m128i zero = _mm_setzero_si128();
					for (; j + 16 <= n; j += 16) {
						__m128i b = _mm_loadu_si128(
							reinterpret_cast<const __m128i*>(src + j));
						__m128i lo = _mm_unpacklo_epi8(b, zero);
						__m128i hi = _mm_unpackhi_epi8(b, zero);
						__m128i w[4] = {
							_mm_unpacklo_epi16(lo, zero),
							_mm_unpackhi_epi16(lo, zero),
							_mm_unpacklo_epi16(hi, zero),
							_mm_unpackhi_epi16(hi, zero) };
						for (int q = 0; q < 4; q++) {
							__m128 f = _mm_mul_ps(_mm_sub_ps(
								_mm_cvtepi32_ps(w[q]), bias), scale);
							_mm_storeu_ps(dst + j + q * 4, f);
						}
					}
#endif
					for (; j < n; j++)
						dst[j] = ((float)src[j] - 127.4f) * (1.0f / 128.0f);
				}
				fDabSync.SetBuffer(&iq[0], iq.size());
				if (requestedSubchannel >= 0
					&& requestedSubchannel != fDabSubchannel) {
					fDabMsc = DabMsc();
					fDmbAudio.Reset();
							fDabSubchannel = -1;
				}
				// Keep all FIC symbols in the live path. The attempted symbol
				// omission reduced CPU but caused a reproducible RS regression
				// on the Atom (52/243 before it, 0 afterwards).
				fDabSync.SetSkipFic(false);
				// Reset MSC at each read boundary so every 6 MiB read is decoded
				// independently. Frame tracking restarts per read (expectedNext=-1),
				// which loses the frame straddling the boundary; carrying the MSC
				// time-interleaver across that gap drifts its phase and RS collapses.
				// Deterministic test on a clean capture (msc_chunk): carry 34% vs
				// per-read reset 100%. Cost is the 16-CIF priming interval per read,
				// a short gap the fade/conceal path in DmbAudio smooths.
				if (fDabMsc.Configured() && dmbPreviousBlock) {
					fDabMsc.Reset();
					fDmbAudio.Reset();
				}
				dmbSoft.resize((size_t)(DabSync::kSymbolsPerFrame - 1)
					* DabSync::kSoftPerSymbol);
				std::vector<float>& soft = dmbSoft;
				int positions[DabSync::kSymbolsPerFrame];
				size_t count = iq.size();
				size_t searchFrom = 0;
				// expectedNext continues frame tracking within this read; it restarts
				// (-1) each read, and the carried tail (assigned at each break below,
				// starting just before a null) makes the CIF stream contiguous across
				// reads. Ported from the macOS _ProcessDab path.
				int expectedNext = -1;
				while (true) {
					float fineHz = 0.0f;
					float depth = 1.0f;
					int start = -1;
					int attemptedExpected = expectedNext;
					if (expectedNext >= 0)
						start = fDabSync.NextFrame(expectedNext, &fineHz);
					if (start < 0)
						start = fDabSync.FindFrame(searchFrom, &fineHz, &depth);
					if (start < 0) {
						break;
					}
					// A frame straddling the read boundary can't be tracked whole in
					// this buffer; stop here and let the next read re-lock its null.
					if ((size_t)start + (size_t)DabSync::kSymbolsPerFrame
							* DabSync::kSymbolSamples > count)
						break;
					if (depth < playbackNullDepth)
						playbackNullDepth = depth;
					playbackFineHz = fineHz;
					searchFrom = (size_t)start
						+ (size_t)(DabSync::kSymbolsPerFrame - 2)
							* DabSync::kSymbolSamples;
					if (!fDabSync.TrackFrame(start, fineHz, positions)) {
						expectedNext = -1;
						// Preserve the time-interleaver order at the exact point of
						// the failed frame. Delaying these four erasure CIFs until the
						// next USB block put later valid frames before the erasure and
						// permanently shifted MSC phase (live RS 152/15461, while each
						// 6 MiB block independently gives about 240/242).
						if (fDabMsc.Configured()) {
							static std::vector<float> erasure(
								DabMsc::kCifBits, 0.0f);
							for (int cif = 0; cif < 4; cif++)
								fDabMsc.PushCif(&erasure[0]);
							const std::vector<uint8>& packets = fDabMsc.Packets();
							if (!packets.empty()) {
								audio.clear();
								fDmbAudio.Feed(&packets[0], packets.size(), audio);
								_CollectDmbVideo();
								fDabMsc.ClearPackets();
								if (!audio.empty()) {
									const DmbAudio::stats& ds = fDmbAudio.Stats();
									if (ds.sampleRate > 0
										&& (fAudio.SampleRate()
												!= (uint32)ds.sampleRate
											|| fAudio.InputChannels() != 1))
										fAudio.Configure((uint32)ds.sampleRate, 1);
									fAudio.Write(&audio[0], audio.size());
									produced += audio.size();
								}
							}
						}
						continue;
					}
					expectedNext = positions[DabSync::kSymbolsPerFrame - 1]
						+ DabSync::kSymbolSamples + DabSync::kNullSamples;
					tracked++;
					fDabSync.SoftBits(positions, fineHz, &soft[0]);
					bool ficReady = !fDabFic.Subchannels().empty();
					const std::vector<DabFic::service>& knownServices
						= fDabFic.Services();
					bool labelledTs = false;
					for (size_t i = 0; i < knownServices.size(); i++) {
						if (!knownServices[i].label.empty()
							&& !knownServices[i].isAudio
							&& knownServices[i].type == 24) {
							labelledTs = true;
							break;
						}
					}
					// Once a TS service is configured, its FIC map is stable; stop
					// spending Atom cycles re-decoding FIC on every eighth frame.
					if (!ficReady || !labelledTs || !fDabMsc.Configured())
						fDabFic.DecodeFrame(&soft[0]);
					dmbFrameCounter++;

					if (!fDabMsc.Configured()) {
						int wanted = requestedSubchannel;
						const std::vector<DabFic::service>& services
							= fDabFic.Services();
						(void)services;
						// Never decode an arbitrary first programme. FIC discovery is
						// allowed with no requested service; MSC starts only after the
						// Station menu supplies an explicit subchannel.
						if (wanted < 0)
							continue;
						const DabFic& map = fDabFic.Subchannels().empty()
							? fDabProbe.Fic() : fDabFic;
						const std::vector<DabFic::subchannel>& subs
							= map.Subchannels();
						for (size_t i = 0; i < subs.size(); i++) {
							if (subs[i].shortForm || subs[i].sizeCu <= 0)
								continue;
							if (wanted >= 0 && subs[i].id != wanted)
								continue;
							DabMsc::config c;
							c.startCu = subs[i].startCu;
							c.sizeCu = subs[i].sizeCu;
							c.protectionLevel = subs[i].protectionLevel;
							c.eepOptionB = subs[i].eepOptionB;
							if (fDabMsc.Configure(c)) {
								// Experimental Atom path: hard-decision Viterbi
								// trades a little correction margin for lower CPU use.
								// Hard-decision Viterbi was benchmarked here: it lowered
								// load to ~180%, but produced RS 0/0 and no AU. Keep the
								// validated soft-decision path as the default.
								fDabMsc.SetHardDecision(false);
								// Quantized soft Viterbi measured ~260% load but RS 0/0;
								// keep the full-float decoder for usable audio.
								fDabMsc.SetQuantizedDecision(false);
								fDabSubchannel = subs[i].id;
								break;
							}
						}
					}

					if (fDabMsc.Configured()) {
						const float* msc = &soft[(size_t)3
							* DabSync::kSoftPerSymbol];
						for (int cif = 0; cif < 4; cif++) {
							fDabMsc.PushCif(msc + (size_t)cif
								* DabMsc::kCifBits);
							const std::vector<uint8>& packets = fDabMsc.Packets();
							if (!packets.empty()) {
								audio.clear();
								fDmbAudio.Feed(&packets[0], packets.size(), audio);
								_CollectDmbVideo();
								fDabMsc.ClearPackets();
							if (!audio.empty()) {
								const DmbAudio::stats& ds = fDmbAudio.Stats();
								if (ds.sampleRate > 0
									&& (fAudio.SampleRate() != (uint32)ds.sampleRate
										|| fAudio.InputChannels() != 1))
									fAudio.Configure((uint32)ds.sampleRate,
										1);
							// DmbAudio output is mono-downmixed PCM, one float/frame.
							fAudio.Write(&audio[0], audio.size());
							// DMB has no waveform view; avoid an unnecessary PCM copy.
							produced += audio.size();
							}
						}
					}
				}
			}
				dmbPreviousBlock = tracked > 0;
			} else {
				// DRM is intentionally unavailable with the R820T2. Let the sound
				// player underrun to silence without manufacturing sample counts.
			}

			// DMB switches to the video card; no spectrum/waterfall is shown.
			// Avoid spending an FFT on display-only data while the OFDM/MSC
			// decoder is already CPU-bound on the Atom.

			bigtime_t cost = system_time() - started;
			bigtime_t represented = (bigtime_t)((double)block.data.size()
				* 500000.0 / (double)fDevice.SampleRate());
			if (represented > 0) {
				float load = (float)cost / (float)represented;
				loadAverage += 0.2f * (load - loadAverage);
			}

			BAutolock lock(fLock);
			fSignalDb = digitalSignalDb;
			// Smooth the per-read RS success rate for the quality gauge. Only
			// meaningful once MSC is configured and a read actually tried blocks.
			if (fDabMsc.Configured()) {
				const DabMsc::stats& q = fDabMsc.Stats();
				float rate = q.blocksTried > 0
					? (float)q.rsOk / (float)q.blocksTried : 0.0f;
				if (fDabQuality < 0.0f)
					fDabQuality = rate;
				else
					fDabQuality += 0.35f * (rate - fDabQuality);
			}
			if (requestedSubchannel < 0) {
				fDabResult = dabGainSweepJustFinished
					? dabGainBestResult : fDabProbe.Result();
			} else {
				// Playback deliberately bypasses DabProbe to avoid duplicating the
				// OFDM FFT. Publish lock/FIC telemetry from the decoder that actually
				// feeds MSC so the UI does not claim "nolock" while audio is playing.
				fDabResult.analyses++;
				fDabResult.locked = tracked > 0;
				if (tracked > 0)
					fDabResult.locks++;
				fDabResult.nullDepth = playbackNullDepth;
				fDabResult.fineOffsetHz = playbackFineHz;
				fDabResult.fibsOk = (uint32)fDabFic.FibsOk();
				fDabResult.fibsTried = (uint32)fDabFic.FibsTried();
			}
			fAudioSamples += produced;
			fDspLoad = loadAverage;

			if (mode == kModeDMB) {
				const DabFic& fic = fDabFic;
				fDabEnsemble = fic.EnsembleLabel();
				fDabServices.clear();
				fDabServiceChoices.clear();
				const std::vector<DabFic::service>& svc = fic.Services();
				for (size_t i = 0; i < svc.size(); i++) {
					if (svc[i].label.empty())
						continue;
					char buf[96];
					if (svc[i].isAudio) {
						snprintf(buf, sizeof(buf), "%s [audio ASCTy %d]",
							svc[i].label.c_str(), svc[i].type);
					} else if (svc[i].type == 24) {
						snprintf(buf, sizeof(buf), "%s [MPEG-2 TS]",
							svc[i].label.c_str());
					} else {
						snprintf(buf, sizeof(buf), "%s [data %d]",
							svc[i].label.c_str(), svc[i].type);
					}
					fDabServices.push_back(buf);
					// Korean T-DMB carries both video and radio programmes as
					// MPEG-2 TS.  Some ensembles mark the primary component's
					// TMId as audio even though the service is still a TS; do not
					// hide those selectable services behind the DAB audio flag.
					if (svc[i].type == 24
						&& svc[i].subchannel >= 0) {
						dab_service_choice choice;
						choice.label = svc[i].label;
						choice.subchannel = svc[i].subchannel;
						fDabServiceChoices.push_back(choice);
					}
				}
				const DmbAudio::stats& audioStats = fDmbAudio.Stats();
				char status[192];
				if (audioStats.decodedUnits > 0) {
					snprintf(status, sizeof(status), "T-DMB subchannel %d: ER-BSAC "
						"%d Hz, %d ch, %llu audio frames decoded",
						fDabSubchannel, audioStats.sampleRate, audioStats.channels,
						(unsigned long long)audioStats.decodedUnits);
				} else if (fDabMsc.Configured()) {
					const DabMsc::stats& mscStats = fDabMsc.Stats();
					snprintf(status, sizeof(status), "T-DMB subchannel %d: %d kbps, "
						"RS %llu/%llu; tracked %d, FIB %llu/%llu; waiting for ER-BSAC audio",
						fDabSubchannel, mscStats.bitrateKbps,
						(unsigned long long)mscStats.rsOk,
						(unsigned long long)mscStats.blocksTried, tracked,
						(unsigned long long)fDabResult.fibsOk,
						(unsigned long long)fDabResult.fibsTried);
				} else if (dabGainSweepJustFinished) {
					snprintf(status, sizeof(status), "T-DMB automatic gain selected "
						"%.1f dB (FIB %u/%u); choose a Station", dabGainBest / 10.0,
						(unsigned)dabGainBestResult.fibsOk,
						(unsigned)dabGainBestResult.fibsTried);
				} else {
					snprintf(status, sizeof(status), "T-DMB: %d frames tracked; "
						"waiting for a T-DMB service", tracked);
				}
				fStatus = status;
				dabGainSweepJustFinished = false;
			}
			continue;
		}

		bigtime_t started = system_time();

		audio.resize(fDemod.MaxAudioSamples(block.data.size()));
		size_t produced = fDemod.Process(&block.data[0], block.data.size(),
			&audio[0], audio.size());

		bigtime_t elapsed = system_time() - started;

		bool useStereo = mode == kModeWFM && fmStereo;
		size_t producedFrames = useStereo ? produced / 2 : produced;
		if (producedFrames > 0) {
			if (useStereo)
				fAudio.WriteStereo(&audio[0], producedFrames);
			else
				fAudio.Write(&audio[0], producedFrames);
			_PublishWaveform(&audio[0], produced);

			// Real time this block represents, versus CPU time it cost.
			bigtime_t blockDuration = (bigtime_t)((double)producedFrames * 1000000.0
				/ (double)(fAudio.SampleRate() > 0 ? fAudio.SampleRate() : 44100));
			if (blockDuration > 0) {
				float load = (float)elapsed / (float)blockDuration;
				loadAverage += 0.1f * (load - loadAverage);
			}
		}

		_PublishSpectrum(fDemod);
		_ScanStep();

		float signalDb = fDemod.SignalLevelDb();
		{
			BAutolock lock(fLock);
			fSignalDb = signalDb;
			fCarrierOffset = fDemod.CarrierOffsetHz();
			fSquelchOpen = fDemod.SquelchOpen();
			fDspLoad = loadAverage;
			fAudioSamples += producedFrames;
		}

		(void)signalDb;
		// No software AGC, deliberately. FM has a constant envelope, so the
		// audio level does not depend on the RF level at all, and AM/SSB have
		// their own audio AGC. A conservative fixed tuner gain costs some
		// signal-to-noise on weak stations and nothing else, while the
		// tuner's own automatic mode clips the ADC. The status line flags
		// clipping so the slider can be used deliberately.
	}
}

void
Receiver::_PublishWaveform(const float* samples, size_t n)
{
	BAutolock lock(fLock);
	if (n >= kWaveformSamples) {
		fWaveform.assign(samples + (n - kWaveformSamples), samples + n);
		return;
	}
	// Shift the old tail left and append.
	size_t keep = kWaveformSamples - n;
	memmove(&fWaveform[0], &fWaveform[n], keep * sizeof(float));
	memcpy(&fWaveform[keep], samples, n * sizeof(float));
}

void
Receiver::SetScanning(bool on)
{
	BAutolock lock(fLock);
	fScanning = on;
	fScanParked = false;
	fScanCentreHz = 0;
	fScanActiveAt = 0;
	fScanSteppedAt = 0;
}

bool
Receiver::IsScanning() const
{
	BAutolock lock(fLock);
	return fScanning;
}

void
Receiver::_ScanStep()
{
	// Air frequencies are silent between transmissions, so scanning is the
	// only practical way to hear "whatever is on". Stepping one 25 kHz channel
	// at a time would take minutes a sweep: a retune only takes effect between
	// two USB reads, which is 248 ms. But the tuner is 264.6 kHz wide, so one
	// tune already shows ten channels at once - the spectrum that was just
	// published is searched instead, and the tuner only moves a window at a
	// time. That is 76 tunes for 118-137 MHz rather than 760.
	const bigtime_t kHoldAfterSignal = 2000000;		// stay 2 s after it ends
	const bigtime_t kMinDwell = 400000;				// let a retune settle
	const uint64 kChannelHz = 25000;
	const uint64 kLowHz = 118000000ULL;
	const uint64 kHighHz = 137000000ULL;
	const float kOpenMarginDb = 10.0f;

	bigtime_t now = system_time();
	uint64 tuneTo = 0;

	{
		BAutolock lock(fLock);
		if (!fScanning || fMode != kModeAir)
			return;

		if (fSquelchOpen) {
			fScanActiveAt = now;
			fScanParked = true;
			return;
		}
		if (fScanParked && now - fScanActiveAt < kHoldAfterSignal)
			return;
		fScanParked = false;
		if (now - fScanSteppedAt < kMinDwell)
			return;
		fScanSteppedAt = now;

		// Noise floor from the median of the published spectrum; a carrier has
		// to stand clear of it rather than of an absolute level, so this keeps
		// working as the gain and the band noise change.
		if (fSpectrumDb.size() < 64 || fSpectrumRate == 0) {
			tuneTo = 0;
		} else {
			std::vector<float> sorted(fSpectrumDb);
			std::sort(sorted.begin(), sorted.end());
			float floorDb = sorted[sorted.size() / 2];

			int bins = (int)fSpectrumDb.size();
			double hzPerBin = (double)fSpectrumRate / (double)bins;
			uint64 centre = fFrequency;
			// Only trust the middle of the window: the band edges are the
			// decimation filter's transition region.
			int usable = (int)((double)fSpectrumRate * 0.4 / hzPerBin);
			int best = -1;
			float bestDb = floorDb + kOpenMarginDb;
			for (int i = bins / 2 - usable; i <= bins / 2 + usable; i++) {
				if (i < 1 || i >= bins - 1)
					continue;
				float v = fSpectrumDb[i];
				if (v <= bestDb)
					continue;
				// A local maximum, so one wide carrier is not counted twice.
				if (v < fSpectrumDb[i - 1] || v < fSpectrumDb[i + 1])
					continue;
				bestDb = v;
				best = i;
			}
			if (best >= 0) {
				double offset = ((double)best - (double)bins / 2.0) * hzPerBin;
				double hz = (double)centre + offset;
				// Snap to the 25 kHz raster the band actually uses.
				uint64 snapped = (uint64)((hz / (double)kChannelHz) + 0.5)
					* kChannelHz;
				if (snapped >= kLowHz && snapped < kHighHz)
					tuneTo = snapped;
			}
			if (tuneTo == 0) {
				// Nothing here: move the window on by most of its width.
				uint64 step = (uint64)((double)fSpectrumRate * 0.8);
				uint64 next = (fScanCentreHz == 0 ? kLowHz : fScanCentreHz)
					+ step;
				if (next >= kHighHz)
					next = kLowHz;
				fScanCentreHz = next;
				tuneTo = next;
			} else
				fScanCentreHz = tuneTo;
		}
	}

	if (tuneTo != 0)
		SetFrequency(tuneTo);
}

void
Receiver::_PublishSpectrum(const Demodulator& demod)
{
	// The FFT is only for the display, so it runs at display rates rather
	// than once per USB block (which would be 32 times a second and pure
	// waste on this CPU).
	bigtime_t now = system_time();
	if (now - fLastSpectrum < 66000)
		return;
	fLastSpectrum = now;

	const std::vector<dsp::Cf>& baseband = demod.LastBaseband();
	if (baseband.size() < (size_t)kSpectrumBins)
		return;

	std::vector<float> mags;
	dsp::PowerSpectrumDb(&baseband[0], baseband.size(), kSpectrumBins, mags);

	BAutolock lock(fLock);
	fSpectrumDb.swap(mags);
	fSpectrumRate = demod.BasebandRate();
	fSpectrumSequence++;
}

void
Receiver::Fetch(snapshot& out) const
{
	BAutolock lock(fLock);
	out.waveform = fWaveform;
	out.spectrumDb = fSpectrumDb;
	out.spectrumRate = fSpectrumRate;
	out.spectrumSequence = fSpectrumSequence;
	out.frequencyHz = fFrequency;
	out.mode = fMode;
	out.signalDb = fSignalDb;
	out.audioPeak = fAudio.PeakLevel();
	out.carrierOffsetHz = fCarrierOffset;
	out.gainTenths = fAppliedGain;
	out.automaticGain = fGain < 0;
	out.directSampling = fDevice.DirectSampling();
	out.squelchOpen = fSquelchOpen;
	out.droppedBlocks = fDevice.DroppedBlocks();
	out.underruns = fAudio.Underruns();
	out.iqBytes = fDevice.BytesRead();
	out.readErrors = fDevice.ReadErrors();
	out.blockBytes = (uint32)fDevice.BlockBytes();
	out.audioSamples = fAudioSamples;
	out.framesPlayed = fAudio.FramesPlayed();
	out.dabLocked = fDabResult.locked;
	out.dabMerDb = fDabResult.merDb;
	out.dabNullDepth = fDabResult.nullDepth;
	out.dabOffsetHz = fDabResult.fineOffsetHz;
	out.dabAnalyses = fDabResult.analyses;
	out.dabLocks = fDabResult.locks;
	out.dabFibsOk = fDabResult.fibsOk;
	out.dabFibsTried = fDabResult.fibsTried;
	out.dabQuality = fDabQuality;
	out.dabEnsemble = fDabEnsemble;
	out.dabServices = fDabServices;
	out.dabServiceChoices = fDabServiceChoices;
	out.scanning = fScanning;
	out.scanParked = fScanParked;
	out.dabSelectedSubchannel = fDabSubchannel;
	out.audioRate = fAudio.SampleRate();
	out.dspLoad = fDspLoad;
	out.status = fStatus;
}
