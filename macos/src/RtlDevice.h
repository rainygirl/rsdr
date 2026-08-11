/*
 * RTL-SDR capture for macOS.
 *
 * Same shape as the Haiku SdrDevice - a capture thread doing synchronous
 * reads, a small pool of blocks, and control changes applied between two reads
 * - but on pthreads instead of the BeAPI.
 *
 * The synchronous choice is not carried over blindly. On Haiku it is forced:
 * rtlsdr_cancel_async() crashes that USB stack and a control transfer cannot
 * get through while bulk transfers are queued. libusb on macOS has neither
 * problem, so async would be fine here. Synchronous is kept anyway because it
 * is the same code path the Haiku app uses, which means a bug found on one is
 * a bug found on both - and because the measurement that matters carried over:
 * the per-read overhead is real on any host, so the transfer has to be long
 * enough to dilute it. See kNarrowBlockBytes in the Haiku SdrDevice.h for the
 * numbers behind that.
 */
#ifndef RSDR_MACOS_RTL_DEVICE_H
#define RSDR_MACOS_RTL_DEVICE_H

#include <pthread.h>

#include <string>
#include <vector>

#include "SupportDefs.h"

struct rtlsdr_dev;

class RtlDevice {
public:
	// Analog modes use synchronous reads so repeated tuning never has to cancel
	// a macOS libusb async transfer. 128 KB dilutes the fixed per-read gap enough
	// to preserve the measured sample rate; the adaptive audio sink absorbs the
	// remaining clock difference. DMB alone keeps the continuous async path.
	static const size_t kNarrowBlockBytes = 131072;
	static const size_t kWideBlockBytes = 6291456;
	static const int kQueueBlocks = 8;

						RtlDevice();
						~RtlDevice();

			status_t	Open(uint32 index = 0);
	static	bool		DevicePresent();
			void		Close();
			bool		IsOpen() const { return fDevice != NULL; }
	const std::string&	Description() const { return fDescription; }

			void		RequestSampleRate(uint32 hz);
			void		RequestFrequency(uint32 hz);
			void		RequestGain(int tenthsDb);		// -1 = tuner auto
			void		RequestPpm(int ppm);
	// Overrides the block size the sample rate would pick. Airband monitoring
	// runs at 2.048 MS/s but wants short blocks for responsiveness, where DAB
	// runs at the same rate and needs enormous ones for interleaver
	// continuity - the rate alone cannot decide it.
			void		RequestBlockBytes(size_t bytes);

			uint32		SampleRate() const;
			uint32		Generation() const;
			uint64		BytesRead() const;
			uint64		DroppedBlocks() const;

			status_t	Start();
			void		Stop();
	// True once the dongle has stopped answering. The capture thread gives up
	// rather than retrying forever, so the UI can offer a reconnect instead of
	// blocking on a join that will not finish.
			bool		DeviceLost() const { return fLost; }

	// Blocks up to timeoutUs for the next captured block.
			bool		NextBlock(std::vector<uint8>& out, uint32& generation,
							bigtime_t timeoutUs);

private:
	static	void*		_ThreadEntry(void* cookie);
	static	void		_AsyncCallback(unsigned char* data, uint32 length,
							void* cookie);
			void		_CaptureLoop();
			void		_ConsumeAsync(const uint8* data, size_t length);
			void		_CancelForControl();
			void		_ApplyPending();
			void		_SetBlockBytes(size_t bytes);

			rtlsdr_dev*	fDevice;
			std::string	fDescription;

			pthread_t	fThread;
			bool		fThreadValid;
			volatile bool fStopRequested;
			bool		fAsyncActive;
	// Gain, rate and frequency are often queued together by one preset. Each
	// request used to cancel the same libusb async run again, racing its
	// completion callback with the following control transfer.
			bool		fCancelIssued;
			bool		fDirectSampling;

	mutable pthread_mutex_t	fLock;
			pthread_cond_t	fCond;

			size_t		fBlockBytes;
	std::vector<uint8>	fPool;
	std::vector<size_t>	fLengths;
	std::vector<uint32>	fGenerations;
	std::vector<uint8>	fScratch;
			int			fReadIndex;
			int			fWriteIndex;
			int			fQueueCount;
			size_t		fAsyncFill;
			int			fAsyncSlot;
			uint32		fAsyncGeneration;

			uint32		fPendingRate;
			uint32		fPendingFreq;
			int			fPendingGain;
			int			fPendingPpm;
			bool		fRateDirty;
			bool		fFreqDirty;
			bool		fGainDirty;
			bool		fPpmDirty;
			size_t		fForcedBlockBytes;
			volatile bool	fLost;

			uint32		fCurrentRate;
			uint32		fGeneration;
			uint64		fBytesRead;
			uint64		fDropped;
};

#endif // RSDR_MACOS_RTL_DEVICE_H
