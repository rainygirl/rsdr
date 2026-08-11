#include "SdrDevice.h"

#include <Autolock.h>

#include <cstdio>
#include <cstring>

#include <rtl-sdr.h>

SdrDevice::SdrDevice()
	:
	fDevice(NULL),
	fThread(-1),
	fStopRequested(false),
	fLock("sdr device"),
	fDataSem(-1),
	fBlockBytes(0),
	fQueueBlocks(0),
	fReadIndex(0),
	fWriteIndex(0),
	fQueueCount(0),
	fPendingRate(0),
	fPendingFreq(0),
	fPendingGain(-1),
	fPendingPpm(0),
	fRateDirty(false),
	fFreqDirty(false),
	fGainDirty(false),
	fPpmDirty(false),
	fCurrentRate(0),
	fCurrentFreq(0),
	fDirectSampling(false),
	fCurrentGain(-1),
	fGeneration(1),
	fDropped(0),
	fReadErrors(0),
	fConsecutiveReadErrors(0),
	fBytesRead(0),
	fCaptureSequence(0)
{
	_SetBlockBytes(kNarrowBlockBytes);
}

void
SdrDevice::_SetBlockBytes(size_t bytes)
{
	if (bytes == fBlockBytes)
		return;

	int blocks = (int)(kPoolBytes / bytes);
	if (blocks < 4)
		blocks = 4;
	if (blocks > 32)
		blocks = 32;

	BAutolock lock(fLock);
	fBlockBytes = bytes;
	fQueueBlocks = blocks;
	fPool.resize((size_t)blocks * bytes);
	fLengths.assign(blocks, 0);
	fGenerations.assign(blocks, 0);
	fSequences.assign(blocks, 0);
	fScratch.resize(bytes);
	fReadIndex = 0;
	fWriteIndex = 0;
	fQueueCount = 0;
}

size_t
SdrDevice::BlockBytes() const
{
	BAutolock lock(fLock);
	return fBlockBytes;
}

SdrDevice::~SdrDevice()
{
	Close();
}

status_t
SdrDevice::Open(uint32 deviceIndex)
{
	if (fDevice != NULL)
		return B_OK;

	uint32 count = rtlsdr_get_device_count();
	if (count == 0)
		return B_DEV_NOT_READY;
	if (deviceIndex >= count)
		return B_BAD_INDEX;

	rtlsdr_dev_t* dev = NULL;
	int err = rtlsdr_open(&dev, deviceIndex);
	if (err < 0 || dev == NULL)
		return B_ERROR;

	fDevice = dev;

	const char* name = rtlsdr_get_device_name(deviceIndex);
	fDeviceName = name != NULL && name[0] != '\0' ? name : "RTL-SDR";

	switch (rtlsdr_get_tuner_type(dev)) {
		case RTLSDR_TUNER_E4000:	fTunerName = "Elonics E4000"; break;
		case RTLSDR_TUNER_FC0012:	fTunerName = "Fitipower FC0012"; break;
		case RTLSDR_TUNER_FC0013:	fTunerName = "Fitipower FC0013"; break;
		case RTLSDR_TUNER_FC2580:	fTunerName = "FCI FC2580"; break;
		case RTLSDR_TUNER_R820T:	fTunerName = "Rafael Micro R820T/R820T2"; break;
		case RTLSDR_TUNER_R828D:	fTunerName = "Rafael Micro R828D"; break;
		default:					fTunerName = "unknown"; break;
	}

	int gainCount = rtlsdr_get_tuner_gains(dev, NULL);
	if (gainCount > 0) {
		std::vector<int> gains(gainCount, 0);
		if (rtlsdr_get_tuner_gains(dev, &gains[0]) == gainCount)
			fGainSteps = gains;
	}

	// Manual gain, and the RTL2832's own digital AGC off. The tuner's
	// automatic mode was measured driving the 8-bit ADC into hard clipping on
	// 6% of samples on a strong local FM station.
	rtlsdr_set_tuner_gain_mode(dev, 1);
	rtlsdr_set_agc_mode(dev, 0);

	if (fDataSem < 0) {
		fDataSem = create_sem(0, "sdr blocks");
		if (fDataSem < 0) {
			rtlsdr_close(dev);
			fDevice = NULL;
			return fDataSem;
		}
	}

	return B_OK;
}

void
SdrDevice::Close()
{
	Stop();

	if (fDataSem >= 0) {
		delete_sem(fDataSem);
		fDataSem = -1;
	}
	if (fDevice != NULL) {
		rtlsdr_close((rtlsdr_dev_t*)fDevice);
		fDevice = NULL;
	}
}

void
SdrDevice::RequestSampleRate(uint32 hz)
{
	BAutolock lock(fLock);
	if (hz == fCurrentRate && !fRateDirty)
		return;
	fPendingRate = hz;
	fRateDirty = true;
}

void
SdrDevice::RequestFrequency(uint32 hz)
{
	BAutolock lock(fLock);
	fPendingFreq = hz;
	fFreqDirty = true;
}

void
SdrDevice::RequestGain(int tenthsDb)
{
	BAutolock lock(fLock);
	fPendingGain = tenthsDb;
	fGainDirty = true;
}

void
SdrDevice::RequestPpmCorrection(int ppm)
{
	BAutolock lock(fLock);
	fPendingPpm = ppm;
	fPpmDirty = true;
}

uint32
SdrDevice::SampleRate() const
{
	BAutolock lock(fLock);
	return fCurrentRate;
}

uint32
SdrDevice::Frequency() const
{
	BAutolock lock(fLock);
	return fCurrentFreq;
}

bool
SdrDevice::DirectSampling() const
{
	BAutolock lock(fLock);
	return fDirectSampling;
}

uint32
SdrDevice::Generation() const
{
	BAutolock lock(fLock);
	return fGeneration;
}

uint64
SdrDevice::DroppedBlocks() const
{
	BAutolock lock(fLock);
	return fDropped;
}

uint64
SdrDevice::ReadErrors() const
{
	BAutolock lock(fLock);
	return fReadErrors;
}

uint32
SdrDevice::ConsecutiveReadErrors() const
{
	BAutolock lock(fLock);
	return fConsecutiveReadErrors;
}

uint64
SdrDevice::BytesRead() const
{
	BAutolock lock(fLock);
	return fBytesRead;
}

status_t
SdrDevice::Start()
{
	if (fDevice == NULL)
		return B_NO_INIT;
	if (fThread >= 0)
		return B_OK;

	fStopRequested = false;
	_DrainQueue();
	{
		BAutolock lock(fLock);
		// Error totals are per capture run. Keeping an old failure forever
		// made a later healthy run look wedged in the UI.
		fReadErrors = 0;
		fConsecutiveReadErrors = 0;
	}

	// Above the demodulator thread. Time spent outside rtlsdr_read_sync() is
	// time the dongle is not transferring; on a single core the demodulator
	// would otherwise delay the next read.
	// Strictly above the demodulator, and high enough that nothing in this
	// app can delay it. The RTL2832's FIFO only holds a few milliseconds, so
	// a late re-issue of the next read loses samples silently - the byte count
	// still looks right. Measured with this thread at the same priority as the
	// demodulator: 230 kS/s delivered against 264.6 requested, a 13% shortfall
	// that drained the audio ring and broke the sound up every two seconds.
	// This thread is I/O bound and spends its life blocked in a USB read, so a
	// real-time priority costs nothing.
	fThread = spawn_thread(&SdrDevice::_ThreadEntry, "sdr capture",
		B_REAL_TIME_DISPLAY_PRIORITY, this);
	if (fThread < 0)
		return fThread;
	resume_thread(fThread);
	return B_OK;
}

void
SdrDevice::Stop()
{
	if (fThread < 0)
		return;

	// Nothing to cancel - this is the whole reason the synchronous API is
	// used. The thread finishes the read it is in (31 ms at most) and exits.
	fStopRequested = true;
	status_t result;
	wait_for_thread(fThread, &result);
	fThread = -1;
	_DrainQueue();
}

status_t
SdrDevice::_ThreadEntry(void* cookie)
{
	static_cast<SdrDevice*>(cookie)->_CaptureLoop();
	return B_OK;
}

void
SdrDevice::_CaptureLoop()
{
	rtlsdr_dev_t* dev = (rtlsdr_dev_t*)fDevice;

	_ApplyPending();
	rtlsdr_reset_buffer(dev);

	while (!fStopRequested) {
		// Between two reads, which is the only moment a control transfer
		// works. See the header.
		_ApplyPending();

		int slot = _ReserveSlot();
		uint8* dst = slot >= 0 ? &fPool[(size_t)slot * fBlockBytes]
			: &fScratch[0];

		int read = 0;
		int err = rtlsdr_read_sync(dev, dst, (int)fBlockBytes, &read);
		if (err < 0 || read <= 0) {
			{
				BAutolock lock(fLock);
				fReadErrors++;
				fConsecutiveReadErrors++;
			}
			// Once a synchronous bulk transfer fails on this Haiku backend the
			// endpoint cannot be recovered in software. Retrying, especially
			// with rtlsdr_reset_buffer(), corrupts USBDeviceHandle's transfer
			// queue and crashes its worker after several attempts. Leave the
			// capture thread cleanly; Stop() can still join it, and the UI tells
			// the user to reconnect the dongle.
			break;
		}

		{
			BAutolock lock(fLock);
			fConsecutiveReadErrors = 0;
			fBytesRead += (uint64)read;
			fCaptureSequence++;
		}

		if (slot >= 0)
			_PublishSlot(slot, (size_t)read, fCaptureSequence);
		else {
			// The demodulator is behind. The read still had to happen - not
			// reading is what actually loses samples - so it went to scratch.
			BAutolock lock(fLock);
			fDropped++;
		}
	}
}

void
SdrDevice::_ApplyPending()
{
	rtlsdr_dev_t* dev = (rtlsdr_dev_t*)fDevice;

	uint32 rate = 0;
	uint32 freq = 0;
	int gain = 0;
	int ppm = 0;
	bool doRate = false;
	bool doFreq = false;
	bool doGain = false;
	bool doPpm = false;

	{
		BAutolock lock(fLock);
		doRate = fRateDirty;
		doFreq = fFreqDirty;
		doGain = fGainDirty;
		doPpm = fPpmDirty;
		rate = fPendingRate;
		freq = fPendingFreq;
		gain = fPendingGain;
		ppm = fPendingPpm;
		fRateDirty = false;
		fFreqDirty = false;
		fGainDirty = false;
		fPpmDirty = false;
	}

	if (!doRate && !doFreq && !doGain && !doPpm)
		return;

	// ppm first: rtlsdr_set_freq_correction retunes internally, so anything
	// after it would otherwise be undone.
	if (doPpm)
		rtlsdr_set_freq_correction(dev, ppm);

	if (doRate && rate > 0) {
		rtlsdr_set_sample_rate(dev, rate);
		uint32 actual = rtlsdr_get_sample_rate(dev);
		{
			BAutolock lock(fLock);
			fCurrentRate = actual != 0 ? actual : rate;
			fGeneration++;
		}
		// Anything already captured belongs to the previous filter chain.
		_DrainQueue();
		// Above a megasample a second the transfer has to be big enough to
		// hold a whole DAB frame - see kWideBlockBytes. Safe here and only
		// here: this runs on the capture thread, between two reads, with the
		// queue already drained, so nothing is pointing into the pool.
		_SetBlockBytes(actual >= 1000000 ? kWideBlockBytes : kNarrowBlockBytes);
		rtlsdr_reset_buffer(dev);
	}

	if (doGain) {
		int previousGain;
		{
			BAutolock lock(fLock);
			previousGain = fCurrentGain;
		}
		if (gain < 0) {
			rtlsdr_set_tuner_gain_mode(dev, 0);
		} else {
			rtlsdr_set_tuner_gain_mode(dev, 1);
			rtlsdr_set_tuner_gain(dev, gain);
		}
		{
			BAutolock lock(fLock);
			fCurrentGain = gain;
		}
		// A queued block was captured at the previous gain.  Keeping it would
		// attribute that block's FIB result to the next automatic-gain
		// candidate.  Gain changes do not alter the DSP format, so leave the
		// generation alone, but discard queued samples and start the tuner FIFO
		// afresh before publishing the next block.
		if (gain != previousGain) {
			_DrainQueue();
			rtlsdr_reset_buffer(dev);
		}
	}

	if (doFreq && freq > 0) {
		// Below the tuner input range, bypass the R820T/R820T2 and sample the
		// RTL2832U Q branch directly. This is the same mode rtl_sdr's `-D 2`
		// option uses for LW/MW/SW. Switching back restores the normal tuner
		// path and its gain policy.
		const bool wantDirect = freq < 28800000U;
		if (wantDirect != fDirectSampling) {
			rtlsdr_set_direct_sampling(dev, wantDirect ? 2 : 0);
			rtlsdr_set_agc_mode(dev, wantDirect ? 1 : 0);
			if (!wantDirect) {
				rtlsdr_set_tuner_gain_mode(dev, fCurrentGain < 0 ? 0 : 1);
				if (fCurrentGain >= 0)
					rtlsdr_set_tuner_gain(dev, fCurrentGain);
			}
			{
				BAutolock lock(fLock);
				fDirectSampling = wantDirect;
				fGeneration++;
			}
			_DrainQueue();
			rtlsdr_reset_buffer(dev);
		}
		rtlsdr_set_center_freq(dev, freq);
		uint32 actual = rtlsdr_get_center_freq(dev);
		BAutolock lock(fLock);
		fCurrentFreq = actual != 0 ? actual : freq;
	}
}

int
SdrDevice::_ReserveSlot()
{
	BAutolock lock(fLock);
	if (fQueueCount >= fQueueBlocks) {
		// Keep latency bounded when DSP falls behind.  Dropping the newest
		// block leaves the demodulator chewing through stale data forever;
		// discard the oldest queued block and let the sequence gap reach the
		// DMB interleaver, which already handles it as erasure CIFs.
		fReadIndex = (fReadIndex + 1) % fQueueBlocks;
		fQueueCount--;
		acquire_sem_etc(fDataSem, 1, B_RELATIVE_TIMEOUT, 0);
		fDropped++;
	}
	return fWriteIndex;
}

void
SdrDevice::_PublishSlot(int slot, size_t length, uint64 sequence)
{
	{
		BAutolock lock(fLock);
		fLengths[slot] = length;
		fGenerations[slot] = fGeneration;
		fSequences[slot] = sequence;
		fWriteIndex = (fWriteIndex + 1) % fQueueBlocks;
		fQueueCount++;
	}
	release_sem(fDataSem);
}

void
SdrDevice::_DrainQueue()
{
	BAutolock lock(fLock);
	while (fQueueCount > 0) {
		fQueueCount--;
		fReadIndex = (fReadIndex + 1) % fQueueBlocks;
		acquire_sem_etc(fDataSem, 1, B_RELATIVE_TIMEOUT, 0);
	}
}

status_t
SdrDevice::NextBlock(Block& block, bigtime_t timeout)
{
	status_t err = acquire_sem_etc(fDataSem, 1, B_RELATIVE_TIMEOUT, timeout);
	if (err != B_OK)
		return err;

	BAutolock lock(fLock);
	if (fQueueCount <= 0)
		return B_ERROR;

	int slot = fReadIndex;
	size_t length = fLengths[slot];
	const uint8* src = &fPool[(size_t)slot * fBlockBytes];
	block.data.assign(src, src + length);
	block.generation = fGenerations[slot];
	block.sequence = fSequences[slot];

	fReadIndex = (fReadIndex + 1) % fQueueBlocks;
	fQueueCount--;
	return B_OK;
}
