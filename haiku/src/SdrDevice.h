/*
 * librtlsdr wrapper with its own capture thread.
 *
 * This uses rtlsdr_read_sync(), which is not what any other RTL-SDR program
 * does. The reasoning took three rewrites to get right, so here it is in
 * full - all of it measured on the target machine.
 *
 * The async API is the normal choice and it delivers samples beautifully:
 * rtl_test at 1058400 S/s reports 45 samples lost per million. It has two
 * fatal problems on this Haiku build.
 *
 *   1. It can only be stopped with rtlsdr_cancel_async(), and that reliably
 *      segfaults libusb. The backend's transfer worker blocks on a semaphore
 *      and then pops the head of a BList; cancelling removes an item from
 *      that list without lowering the semaphore, so the worker eventually
 *      wakes with nothing to pop and dereferences NULL in
 *      USBTransfer::Do(). Every single attempt left a debug report on the
 *      Desktop - on clean exits, on killed processes, and on a deliberate
 *      cancel-and-restart test.
 *
 *   2. A control transfer issued while bulk transfers are queued does not
 *      work at all. Every rtlsdr_set_tuner_gain() during a running async
 *      stream returned -1 after timing out for 1.8 to 2.4 seconds, with
 *      register access reporting B_TIMED_OUT throughout, and after a few of
 *      them the stream itself died ("cb transfer status: 1, canceling...").
 *      set_center_freq behaves identically. So an async stream cannot be
 *      retuned, and it cannot be stopped in order to retune either.
 *
 * Synchronous reads have neither problem. Nothing needs cancelling - the
 * thread simply stops calling read_sync - and the bus is idle between calls,
 * which is exactly when a tuner write gets through: measured at 53 ms for a
 * frequency and gain change together, with the next read succeeding, five
 * times in a row, no errors.
 *
 * The one thing synchronous reads cost is samples lost in the gap between
 * transfers, and that is entirely a function of the sample rate:
 *
 *              1058400 S/s        264600 S/s
 *    16 KB       76.1%              100.0%
 *    32 KB       87.1%              100.0%
 *    64 KB       92.7%              100.0%
 *   256 KB       97.7%              100.0%
 *
 * Those 264600 figures come from `test/usb_rate`, and they are true of that
 * program and misleading about this one. usb_rate does nothing between reads.
 * The receiver reserves a pool slot, publishes it, releases a semaphore and
 * waits to be scheduled again, and the dongle keeps filling its FIFO through
 * all of it. Measured in the full receiver at 264600 with 16 KB transfers:
 * 237 kS/s delivered, 90%, in exact multiples of one 8192-sample block - and
 * that shortfall drained the audio ring and broke the sound up every couple of
 * seconds. The fix is transfers long enough for the fixed per-read cost to
 * disappear into them (99.1% at 64 KB, see kNarrowBlockBytes), plus rate
 * control in AudioSink for the remainder, because no buffer size can absorb a
 * steady rate difference - it only changes how long the ring takes to run dry.
 */
#ifndef RSDR_SDR_DEVICE_H
#define RSDR_SDR_DEVICE_H

#include <Locker.h>
#include <OS.h>
#include <SupportDefs.h>

#include <string>
#include <vector>

struct rtlsdr_dev;

class SdrDevice {
public:
	// Transfer size for the narrowband rate: 65536 I/Q pairs, 248 ms at
	// 264600 S/s.
	//
	// This was 16384 bytes, and that is too small once the whole receiver is
	// running rather than a bare USB loop. Every read cycle carries a fixed
	// overhead - reserving a slot, publishing it, releasing the semaphore, and
	// getting scheduled again - measured at about 3 ms here. The dongle keeps
	// filling its FIFO during that gap and silently overflows, so the loss is
	// the overhead divided by the read duration: 3 ms against a 31 ms read is
	// 10%, which is what the delivered rate showed (237 kS/s against 264.6
	// requested, in exact multiples of one 8192-sample block). The same 3 ms
	// against a 124 ms read is under 3%.
	//
	// A bare `usb_rate` loop reports 100% at 16 KB precisely because it has
	// none of that per-block work, which is why the old measurement did not
	// predict this.
	static const size_t kNarrowBlockBytes = 131072;

	// Transfer size at DAB rates: sixteen whole DAB Mode I frames.
	//
	// Not a tuning choice. OFDM synchronisation cannot bridge a gap, and
	// synchronous reads do lose samples between transfers at 2.048 MS/s. Two
	// The MSC time interleaver spans sixteen CIFs (four frames), so a short
	// probe-sized transfer can never produce audio. Sixteen frames leaves
	// enough contiguous data for acquisition, interleaver warm-up and useful
	// output whatever the block alignment.
	//
	// 16 x 196608 samples = 6291456 bytes, and a multiple of 512 as libusb bulk
	// requires. A 12 MiB (32-frame) size was tried to amortise the per-read
	// reset startup, but read_sync splits a large request into URBs and loses
	// samples between them (see the header note), so a longer read only
	// accumulated more phase drift (per-read RS fell from ~100% to 30-65%).
	// The gap is per-transfer, not per-call, so a bigger transfer does not help.
	static const size_t kWideBlockBytes = 6291456;

	// Memory the block queue may use. The block count follows from this and
	// the transfer size, so wide mode gets fewer, larger blocks.
	static const size_t kPoolBytes = 24u * 1024 * 1024;

	struct Block {
		std::vector<uint8>	data;
		// Bumped whenever the sample rate changes, so the consumer can throw
		// away blocks captured with the previous filter chain.
		uint32				generation;
		uint64				sequence;
	};

								SdrDevice();
								~SdrDevice();

			status_t			Open(uint32 deviceIndex = 0);
			void				Close();
			bool				IsOpen() const { return fDevice != NULL; }

			const std::string&	DeviceName() const { return fDeviceName; }
			const std::string&	TunerName() const { return fTunerName; }
	// Gains in tenths of a dB, as librtlsdr reports them.
			const std::vector<int>& GainSteps() const { return fGainSteps; }

	// Safe to call from any thread at any time. These only record the
	// request; the capture thread performs the control transfer between two
	// reads, where it is known to work. A caller therefore never blocks for
	// the ~50 ms a tuner write takes, and two control transfers can never
	// overlap.
			void				RequestSampleRate(uint32 hz);
			void				RequestFrequency(uint32 hz);
			void				RequestGain(int tenthsDb);	// -1 = tuner auto
			void				RequestPpmCorrection(int ppm);

			uint32				SampleRate() const;
			uint32				Frequency() const;
			bool				DirectSampling() const;

			status_t			Start();
			void				Stop();
			bool				IsRunning() const { return fThread >= 0; }

	// Blocks until a captured block is available or the timeout expires.
			status_t			NextBlock(Block& block, bigtime_t timeout);
			void				Flush() { _DrainQueue(); }
			size_t				BlockBytes() const;

			uint32				Generation() const;
			uint64				DroppedBlocks() const;
			uint64				ReadErrors() const;
			uint32				ConsecutiveReadErrors() const;
	// Total bytes the dongle actually delivered. Against elapsed time and the
	// sample rate this says whether capture is keeping up; samples the
	// RTL2832 dropped internally are invisible everywhere else.
			uint64				BytesRead() const;

private:
	static	status_t			_ThreadEntry(void* cookie);
			void				_CaptureLoop();
			void				_ApplyPending();
			int					_ReserveSlot();
			void				_PublishSlot(int slot, size_t length, uint64 sequence);
			void				_DrainQueue();
	// Resizes the block pool for a new transfer size. Only ever called from
	// the capture thread between two reads, with the queue already drained.
			void				_SetBlockBytes(size_t bytes);


			rtlsdr_dev*			fDevice;
			std::string			fDeviceName;
			std::string			fTunerName;
			std::vector<int>	fGainSteps;

			thread_id			fThread;
			volatile bool		fStopRequested;

	mutable BLocker				fLock;
			sem_id				fDataSem;

			size_t				fBlockBytes;
			int					fQueueBlocks;
			std::vector<uint8>	fPool;
			std::vector<size_t>	fLengths;
	std::vector<uint32>	fGenerations;
	std::vector<uint64>	fSequences;
			std::vector<uint8>	fScratch;   // sink for blocks with nowhere to go
			int					fReadIndex;
			int					fWriteIndex;
			int					fQueueCount;

			uint32				fPendingRate;
			uint32				fPendingFreq;
			int					fPendingGain;
			int					fPendingPpm;
			bool				fRateDirty;
			bool				fFreqDirty;
			bool				fGainDirty;
			bool				fPpmDirty;

			uint32				fCurrentRate;
			uint32				fCurrentFreq;
			bool				fDirectSampling;
			int					fCurrentGain;
			uint32				fGeneration;
			uint64				fDropped;
			uint64				fReadErrors;
			uint32				fConsecutiveReadErrors;
			uint64				fBytesRead;
			uint64				fCaptureSequence;
};

#endif // RSDR_SDR_DEVICE_H
