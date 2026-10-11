/*
 * Owns the dongle, the demodulator and the sound output, and runs the thread
 * that connects them.
 *
 * Three threads are involved:
 *   - SdrDevice's capture thread loops on rtlsdr_read_sync() and applies any
 *     pending tuner change between two reads, which is the only moment a
 *     control transfer works on this Haiku build (see SdrDevice.h).
 *   - This class's demodulator thread pulls blocks and does all the DSP.
 *   - BSoundPlayer's play thread drains the audio ring.
 * The UI thread never touches any of them; it polls Fetch() on a timer, and
 * its setters only leave a note for the capture thread to pick up.
 */
#ifndef RSDR_RECEIVER_H
#define RSDR_RECEIVER_H

#include <Locker.h>
#include <OS.h>
#include <SupportDefs.h>

#include <string>
#include <vector>

#include "AudioSink.h"
#include "DabFic.h"
#include "DabMsc.h"
#include "DabProbe.h"
#include "DabSync.h"
#include "Demodulator.h"
#include "DmbAudio.h"
#include "SdrDevice.h"

class Receiver {
public:
	// Tuner gain in tenths of a dB, and one of the R820T2's actual 29 steps.
	// Deliberately well short of the 49.6 dB maximum: the dongle's own
	// automatic mode was measured driving the 8-bit ADC into hard clipping on
	// 6% of samples on a strong local FM station, which sounds like
	// electrical crackle and cannot be filtered out afterwards. FM's constant
	// envelope means a lower gain costs signal-to-noise and nothing else.
	//
	// A low-gain experiment (27 tenths) was tried and reverted: live
	// live_check on 8B mYTN showed lower gain only lowers MER (8.4 dB at 166
	// vs 11.3 dB at 360) and does not help - FIC locks 12/12 at every gain,
	// but audio never flows because the DSP load ramps past real time
	// (48 -> 153% over 20 s, drops accumulating). The blocker is the section-5
	// CPU wall in the OFDM front end, not tuner gain.
	static const int kDefaultGainTenths = 254;

	struct dab_service_choice {
		std::string label;
		int subchannel;
		dab_service_choice() : subchannel(-1) {}
	};

	struct snapshot {
		std::vector<float>	waveform;		// most recent audio samples
		std::vector<float>	spectrumDb;
		uint32				spectrumRate;	// span of spectrumDb, in Hz
	// Bumped each time spectrumDb is actually recomputed. The waterfall needs
	// it: in DMB mode one USB read covers 1.536 s, so new spectrum data arrives
	// much less often than the UI polls - without this
	// the waterfall wrote the same line six times over and looked frozen.
		uint32				spectrumSequence;
		uint64				frequencyHz;
		demod_mode			mode;
		float				signalDb;
		float				audioPeak;
		float				carrierOffsetHz;	// FM modes only
		int					gainTenths;			// what is actually set
		bool				automaticGain;
		bool				directSampling;
		bool				squelchOpen;
		bool				scanning;
		bool				scanParked;
		uint64				droppedBlocks;
		uint64				underruns;
		uint64				iqBytes;		// delivered by the dongle
		uint64				readErrors;		// USB reads that failed outright
		uint32				blockBytes;		// USB transfer size in use
		uint64				audioSamples;	// produced by the demodulator
		uint64				framesPlayed;	// consumed by the sound card
	// DMB mode only, from DabProbe. Reception indication, not decoding.
		bool				dabLocked;
		float				dabMerDb;
		float				dabNullDepth;
		float				dabOffsetHz;
		uint32				dabAnalyses;
		uint32				dabLocks;
		uint32				dabFibsOk;
		uint32				dabFibsTried;
	// Smoothed MSC RS success rate 0-1: the metric that predicts audio, for the
	// signal-quality gauge. -1 before the first configured read.
		float				dabQuality;
		std::string			dabEnsemble;
	// "label (kind, subchannel)" per service, ready to display.
	std::vector<std::string> dabServices;
	std::vector<dab_service_choice> dabServiceChoices;
		int				dabSelectedSubchannel;
		uint32				audioRate;
		float				dspLoad;		// 0.0-1.0 of real time
		std::string			status;

		snapshot()
			:
			spectrumRate(0),
			spectrumSequence(0),
			frequencyHz(0),
			mode(kModeWFM),
			signalDb(-120.0f),
			audioPeak(0.0f),
			carrierOffsetHz(0.0f),
			gainTenths(0),
				automaticGain(true),
				directSampling(false),
			squelchOpen(true),
			scanning(false),
			scanParked(false),
			droppedBlocks(0),
			underruns(0),
			iqBytes(0),
			readErrors(0),
			blockBytes(0),
			audioSamples(0),
			framesPlayed(0),
			dabLocked(false),
			dabMerDb(0.0f),
			dabNullDepth(1.0f),
			dabOffsetHz(0.0f),
			dabAnalyses(0),
			dabLocks(0),
			dabFibsOk(0),
				dabFibsTried(0),
				dabQuality(-1.0f),
				dabSelectedSubchannel(-1),
			audioRate(0),
			dspLoad(0.0f)
		{
		}
	};

							Receiver();
							~Receiver();

			status_t		Start();
			void			Stop();
			bool			IsRunning() const { return fThread >= 0; }

			std::string		DeviceDescription() const;
	const std::vector<int>&	GainSteps() const { return fDevice.GainSteps(); }

			void			SetMode(demod_mode mode);
			demod_mode		Mode() const;
			void			SetFrequency(uint64 hz);
			uint64			Frequency() const;
			void			SetFineTune(float hz);
			float			FineTune() const;
			void			SetGain(int tenthsDb);	// -1 for automatic
			int				Gain() const;
			void			SetSquelchDb(float db);
			void			SetFmStereo(bool enabled);
	// Airband scanning. Steps the tuner across the band plan and parks on any
	// channel carrying a signal, which is the only practical way to use an air
	// frequency: they are silent except while somebody is transmitting.
			void			SetScanning(bool on);
			bool			IsScanning() const;
			void			SetVolume(float gain);
			float			Volume() const { return fAudio.Volume(); }
	// Crystal error correction, in ppm, passed straight to
	// rtlsdr_set_freq_correction. See snapshot::carrierOffsetHz for how to
	// find the right value for a given dongle.
			void			SetPpm(int ppm);
			int				Ppm() const;
			void			SetDabSubchannel(int subchannel);
			void			TakeDmbVideo(std::vector<uint8>& config,
								std::vector<DmbAudio::video_unit>& units);

			void			Fetch(snapshot& out) const;

private:
	static	status_t		_ThreadEntry(void* cookie);
			void			_DemodLoop();

			void			_PublishWaveform(const float* samples, size_t n);
			void			_PublishSpectrum(const Demodulator& demod);
			void			_CollectDmbVideo();
	// One scanner step, driven by the spectrum that was just published.
			void			_ScanStep();

	static const size_t		kWaveformSamples = 2048;
	static const int		kSpectrumBins = 1024;

			SdrDevice		fDevice;
			Demodulator		fDemod;
			AudioSink		fAudio;
			DabProbe		fDabProbe;
			DabSync			fDabSync;
			DabFic			fDabFic;
			DabMsc			fDabMsc;
			DmbAudio		fDmbAudio;
			int				fDabSubchannel;
			int				fRequestedDabSubchannel;
			// Smoothed RS success rate (0-1) for the signal-quality gauge; -1
			// until the first configured read produces RS statistics.
			float			fDabQuality;

			thread_id		fThread;
			bool			fStopRequested;

	mutable BLocker			fLock;

	// Requested state, written by the UI thread, consumed by the demod
	// thread.
			demod_mode		fMode;
			uint64			fFrequency;
			float			fFineTune;
			int				fGain;
			int				fPpm;
			float			fSquelchDb;
			bool			fFmStereo;
			bool			fModeDirty;

	// Published state, written by the demod thread.
	std::vector<float>		fWaveform;
	std::vector<float>		fSpectrumDb;
			uint32			fSpectrumRate;
			uint32			fSpectrumSequence;
			float			fSignalDb;
			float			fCarrierOffset;
			DabProbe::result fDabResult;
			std::string		fDabEnsemble;
	std::vector<std::string> fDabServices;
	std::vector<dab_service_choice> fDabServiceChoices;
	std::vector<uint8>		fDmbVideoConfig;
	std::vector<DmbAudio::video_unit> fDmbVideoUnits;
	bool					fDmbAudioOnly;
	bool					fDmbVideoSnapshotTaken;
	// Wall-clock bookkeeping for the silence fed to the sound card in the
	// digital modes, where the sample stream is deliberately discontinuous.
			bigtime_t		fLastDigitalAudio;
			double			fDigitalAudioDebt;
			bool			fSquelchOpen;
			bool			fScanning;
			bool			fScanParked;
			uint64			fScanCentreHz;
			bigtime_t		fScanActiveAt;
			bigtime_t		fScanSteppedAt;
			float			fDspLoad;
			uint64			fAudioSamples;
			int				fAppliedGain;
			std::string		fStatus;
			bigtime_t		fLastSpectrum;
};

#endif // RSDR_RECEIVER_H
