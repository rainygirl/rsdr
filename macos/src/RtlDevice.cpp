#include "RtlDevice.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <unistd.h>
#include <pthread/qos.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

#include <rtl-sdr.h>

RtlDevice::RtlDevice()
	:
	fDevice(NULL),
	fThreadValid(false),
	fStopRequested(false),
	fAsyncActive(false),
	fCancelIssued(false),
	fDirectSampling(false),
	fBlockBytes(0),
	fReadIndex(0),
	fWriteIndex(0),
	fQueueCount(0),
	fAsyncFill(0),
	fAsyncSlot(-1),
	fAsyncGeneration(0),
	fPendingRate(0),
	fPendingFreq(0),
	fPendingGain(-1),
	fPendingPpm(0),
	fRateDirty(false),
	fFreqDirty(false),
	fGainDirty(false),
	fPpmDirty(false),
	fForcedBlockBytes(0),
	fLost(false),
	fCurrentRate(0),
	fGeneration(0),
	fBytesRead(0),
	fDropped(0)
{
	pthread_mutex_init(&fLock, NULL);
	pthread_cond_init(&fCond, NULL);
	_SetBlockBytes(kNarrowBlockBytes);
}

RtlDevice::~RtlDevice()
{
	Stop();
	Close();
	pthread_cond_destroy(&fCond);
	pthread_mutex_destroy(&fLock);
}

bool
RtlDevice::DevicePresent()
{
	// Never call rtlsdr_get_device_count() from the UI polling timer. That API
	// creates and destroys a libusb context on every call; on macOS a detach
	// notification can make libusb_exit() wait forever for its own hotplug
	// thread. Query the I/O Registry instead, independently of the context used
	// by the live receiver.
	static const struct { uint16_t vendor; uint16_t product; } known[] = {
		{0x0bda, 0x2832}, {0x0bda, 0x2838},
		{0x0ccd, 0x00a9}, {0x0ccd, 0x00b3}, {0x0ccd, 0x00b4},
		{0x0ccd, 0x00b5}, {0x0ccd, 0x00b7}, {0x0ccd, 0x00b8},
		{0x0ccd, 0x00b9}, {0x0ccd, 0x00c0}, {0x0ccd, 0x00c6},
		{0x0ccd, 0x00d3}, {0x0ccd, 0x00d7}, {0x0ccd, 0x00e0},
		{0x185b, 0x0620}, {0x185b, 0x0650},
		{0x1b80, 0xd393}, {0x1b80, 0xd394}, {0x1b80, 0xd395},
		{0x1b80, 0xd397}, {0x1b80, 0xd398}, {0x1b80, 0xd39d},
		{0x1d19, 0x1101}, {0x1d19, 0x1102}, {0x1d19, 0x1103},
		{0x1f4d, 0xa803}, {0x1f4d, 0xb803}, {0x1f4d, 0xc803},
		{0x1f4d, 0xd286}, {0x0458, 0x707f}
	};

	io_iterator_t iterator = IO_OBJECT_NULL;
	CFMutableDictionaryRef match = IOServiceMatching("IOUSBHostDevice");
	if (match == NULL || IOServiceGetMatchingServices(kIOMainPortDefault,
			match, &iterator) != KERN_SUCCESS)
		return false;

	bool found = false;
	io_service_t service;
	while (!found && (service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
		CFTypeRef vendorRef = IORegistryEntryCreateCFProperty(service,
			CFSTR("idVendor"), kCFAllocatorDefault, 0);
		CFTypeRef productRef = IORegistryEntryCreateCFProperty(service,
			CFSTR("idProduct"), kCFAllocatorDefault, 0);
		int vendor = -1, product = -1;
		if (vendorRef != NULL && CFGetTypeID(vendorRef) == CFNumberGetTypeID())
			CFNumberGetValue((CFNumberRef)vendorRef, kCFNumberIntType, &vendor);
		if (productRef != NULL && CFGetTypeID(productRef) == CFNumberGetTypeID())
			CFNumberGetValue((CFNumberRef)productRef, kCFNumberIntType, &product);
		if (vendorRef != NULL)
			CFRelease(vendorRef);
		if (productRef != NULL)
			CFRelease(productRef);
		for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
			if (vendor == known[i].vendor && product == known[i].product) {
				found = true;
				break;
			}
		}
		IOObjectRelease(service);
	}
	IOObjectRelease(iterator);
	return found;
}

void
RtlDevice::_SetBlockBytes(size_t bytes)
{
	if (bytes == fBlockBytes)
		return;
	pthread_mutex_lock(&fLock);
	fBlockBytes = bytes;
	fPool.resize((size_t)kQueueBlocks * bytes);
	fLengths.assign(kQueueBlocks, 0);
	fGenerations.assign(kQueueBlocks, 0);
	fScratch.resize(bytes);
	fReadIndex = 0;
	fWriteIndex = 0;
	fQueueCount = 0;
	fAsyncFill = 0;
	fAsyncSlot = -1;
	pthread_mutex_unlock(&fLock);
}

status_t
RtlDevice::Open(uint32 index)
{
	if (fDevice != NULL)
		return B_OK;

	// Do not preflight with rtlsdr_get_device_count(). librtlsdr implements
	// that call by creating and immediately destroying a temporary libusb
	// context. On macOS the Darwin hotplug run loop can still be tearing that
	// context down when the following rtlsdr_open() creates another one. Two
	// observed Play-button crashes entered darwin_submit_transfer from
	// rtlsdr_open() with that half-torn-down state. DevicePresent() already does
	// UI polling through IOKit; here a single rtlsdr_open() is both the probe and
	// the open operation.
	rtlsdr_dev_t* dev = NULL;
	if (rtlsdr_open(&dev, index) < 0 || dev == NULL)
		return B_DEV_NOT_READY;
	fDevice = (rtlsdr_dev*)dev;
	fDirectSampling = false;

	const char* name = rtlsdr_get_device_name(index);
	fDescription = name != NULL ? name : "RTL-SDR";
	switch (rtlsdr_get_tuner_type(dev)) {
		case RTLSDR_TUNER_E4000:  fDescription += " / E4000"; break;
		case RTLSDR_TUNER_FC0012: fDescription += " / FC0012"; break;
		case RTLSDR_TUNER_FC0013: fDescription += " / FC0013"; break;
		case RTLSDR_TUNER_FC2580: fDescription += " / FC2580"; break;
		case RTLSDR_TUNER_R820T:  fDescription += " / R820T/R820T2"; break;
		case RTLSDR_TUNER_R828D:  fDescription += " / R828D"; break;
		default: fDescription += " / unknown tuner"; break;
	}

	rtlsdr_set_agc_mode(dev, 0);
	rtlsdr_set_tuner_gain_mode(dev, 1);
	return B_OK;
}

void
RtlDevice::Close()
{
	if (fDevice == NULL)
		return;
	rtlsdr_close((rtlsdr_dev_t*)fDevice);
	fDevice = NULL;
}

void
RtlDevice::RequestSampleRate(uint32 hz)
{
	pthread_mutex_lock(&fLock);
	fPendingRate = hz;
	fRateDirty = true;
	pthread_mutex_unlock(&fLock);
	_CancelForControl();
}

void
RtlDevice::RequestFrequency(uint32 hz)
{
	pthread_mutex_lock(&fLock);
	fPendingFreq = hz;
	fFreqDirty = true;
	pthread_mutex_unlock(&fLock);
	_CancelForControl();
}

void
RtlDevice::RequestGain(int tenthsDb)
{
	pthread_mutex_lock(&fLock);
	fPendingGain = tenthsDb;
	fGainDirty = true;
	pthread_mutex_unlock(&fLock);
	_CancelForControl();
}

void
RtlDevice::RequestBlockBytes(size_t bytes)
{
	pthread_mutex_lock(&fLock);
	fForcedBlockBytes = bytes;
	pthread_mutex_unlock(&fLock);
	_CancelForControl();
}

void
RtlDevice::RequestPpm(int ppm)
{
	pthread_mutex_lock(&fLock);
	fPendingPpm = ppm;
	fPpmDirty = true;
	pthread_mutex_unlock(&fLock);
	_CancelForControl();
}

void
RtlDevice::_CancelForControl()
{
	// Unlike Haiku's libusb backend, macOS safely supports cancelling async
	// transfers. Wake the capture thread so tuner controls are applied between
	// async runs instead of waiting forever for a stream that never returns.
	pthread_mutex_lock(&fLock);
	bool cancel = fThreadValid && fAsyncActive && !fCancelIssued
		&& fDevice != NULL;
	if (cancel)
		fCancelIssued = true;
	pthread_mutex_unlock(&fLock);
	if (cancel)
		rtlsdr_cancel_async((rtlsdr_dev_t*)fDevice);
}

uint32
RtlDevice::SampleRate() const
{
	pthread_mutex_lock(&fLock);
	uint32 r = fCurrentRate;
	pthread_mutex_unlock(&fLock);
	return r;
}

uint32
RtlDevice::Generation() const
{
	pthread_mutex_lock(&fLock);
	uint32 g = fGeneration;
	pthread_mutex_unlock(&fLock);
	return g;
}

uint64
RtlDevice::BytesRead() const
{
	pthread_mutex_lock(&fLock);
	uint64 v = fBytesRead;
	pthread_mutex_unlock(&fLock);
	return v;
}

uint64
RtlDevice::DroppedBlocks() const
{
	pthread_mutex_lock(&fLock);
	uint64 v = fDropped;
	pthread_mutex_unlock(&fLock);
	return v;
}

status_t
RtlDevice::Start()
{
	if (fDevice == NULL)
		return B_ERROR;
	if (fThreadValid)
		return B_OK;
	fStopRequested = false;
	fLost = false;

	// The capture thread must never be late re-issuing a read. The RTL2832's
	// FIFO holds only a few milliseconds, and when it overflows the samples
	// are gone silently - the byte count still looks right. Measured in the
	// app against the same code run standalone: null depth 0.32 against 0.17
	// and FIC 47% against 100%, purely from this thread losing the CPU to the
	// audio queue and the window while a 1.5 s read was in progress.
	//
	// It spends its life blocked in libusb, so a high class costs nothing.
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
	int rc = pthread_create(&fThread, &attr, &RtlDevice::_ThreadEntry, this);
	pthread_attr_destroy(&attr);
	if (rc != 0)
		return B_ERROR;
	fThreadValid = true;
	return B_OK;
}

void
RtlDevice::Stop()
{
	if (!fThreadValid)
		return;
	// Serialize with the capture thread's transition into read_async. Without
	// this handshake Stop could check fAsyncActive just before it became true,
	// miss the cancellation, then wait forever in pthread_join.
	pthread_mutex_lock(&fLock);
	fStopRequested = true;
	bool cancel = fAsyncActive && !fCancelIssued;
	if (cancel)
		fCancelIssued = true;
	pthread_mutex_unlock(&fLock);
	if (cancel)
		rtlsdr_cancel_async((rtlsdr_dev_t*)fDevice);
	pthread_join(fThread, NULL);
	fThreadValid = false;
	// On macOS, rtlsdr_read_async() can return before libusb's separate
	// darwin/hotplug run loop has dispatched its final I/O completion. Closing
	// the handle or starting control transfers immediately then gives that
	// callback a freed transfer (darwin_async_io_callback at address 0x30).
	// Let that run loop drain before Stop() advertises that the device is idle.
	usleep(250000);

	pthread_mutex_lock(&fLock);
	fQueueCount = 0;
	fReadIndex = 0;
	fWriteIndex = 0;
	fAsyncFill = 0;
	fAsyncSlot = -1;
	pthread_mutex_unlock(&fLock);
}

void*
RtlDevice::_ThreadEntry(void* cookie)
{
	static_cast<RtlDevice*>(cookie)->_CaptureLoop();
	return NULL;
}

void
RtlDevice::_ApplyPending()
{
	rtlsdr_dev_t* dev = (rtlsdr_dev_t*)fDevice;

	uint32 rate, freq;
	int gain, ppm;
	bool doRate, doFreq, doGain, doPpm;

	pthread_mutex_lock(&fLock);
	doRate = fRateDirty; doFreq = fFreqDirty;
	doGain = fGainDirty; doPpm = fPpmDirty;
	rate = fPendingRate; freq = fPendingFreq;
	gain = fPendingGain; ppm = fPendingPpm;
	fRateDirty = fFreqDirty = fGainDirty = fPpmDirty = false;
	pthread_mutex_unlock(&fLock);

	if (!doRate && !doFreq && !doGain && !doPpm)
		return;

	// ppm first: set_freq_correction retunes internally.
	if (doPpm)
		rtlsdr_set_freq_correction(dev, ppm);

	if (doRate && rate > 0) {
		rtlsdr_set_sample_rate(dev, rate);
		uint32 actual = rtlsdr_get_sample_rate(dev);
		pthread_mutex_lock(&fLock);
		fCurrentRate = actual != 0 ? actual : rate;
		fGeneration++;
		fQueueCount = 0;
		fReadIndex = 0;
		fWriteIndex = 0;
		pthread_mutex_unlock(&fLock);
		size_t forced;
		pthread_mutex_lock(&fLock);
		forced = fForcedBlockBytes;
		pthread_mutex_unlock(&fLock);
		_SetBlockBytes(forced != 0 ? forced
			: (actual >= 1000000 ? kWideBlockBytes : kNarrowBlockBytes));
		rtlsdr_reset_buffer(dev);
	}

	if (doGain) {
		if (gain < 0)
			rtlsdr_set_tuner_gain_mode(dev, 0);
		else {
			rtlsdr_set_tuner_gain_mode(dev, 1);
			rtlsdr_set_tuner_gain(dev, gain);
		}
	}

	if (doFreq && freq > 0) {
		// Below the tuner's input range, sample the RTL2832U Q branch instead.
		bool wantDirect = freq < 28800000U;
		if (wantDirect != fDirectSampling) {
			rtlsdr_set_direct_sampling(dev, wantDirect ? 2 : 0);
			rtlsdr_set_agc_mode(dev, wantDirect ? 1 : 0);
			fDirectSampling = wantDirect;
			rtlsdr_reset_buffer(dev);
		}
		rtlsdr_set_center_freq(dev, freq);
		pthread_mutex_lock(&fLock);
		fGeneration++;
		fQueueCount = 0;
		fReadIndex = 0;
		fWriteIndex = 0;
		fAsyncFill = 0;
		fAsyncSlot = -1;
		pthread_mutex_unlock(&fLock);
		rtlsdr_reset_buffer(dev);
	}
}

void
RtlDevice::_AsyncCallback(unsigned char* data, uint32 length, void* cookie)
{
	static_cast<RtlDevice*>(cookie)->_ConsumeAsync(data, length);
}

void
RtlDevice::_ConsumeAsync(const uint8* data, size_t length)
{
	if (fStopRequested || data == NULL || length == 0)
		return;
	pthread_mutex_lock(&fLock);
	while (length > 0) {
		if (fAsyncFill == 0) {
			fAsyncSlot = fQueueCount < kQueueBlocks ? fWriteIndex : -1;
			fAsyncGeneration = fGeneration;
		}
		uint8* dst = fAsyncSlot >= 0
			? &fPool[(size_t)fAsyncSlot * fBlockBytes] : &fScratch[0];
		size_t take = fBlockBytes - fAsyncFill;
		if (take > length)
			take = length;
		memcpy(dst + fAsyncFill, data, take);
		fAsyncFill += take;
		data += take;
		length -= take;
		if (fAsyncFill != fBlockBytes)
			continue;

		fBytesRead += fBlockBytes;
		if (fAsyncSlot >= 0) {
			fLengths[fAsyncSlot] = fBlockBytes;
			fGenerations[fAsyncSlot] = fAsyncGeneration;
			fWriteIndex = (fWriteIndex + 1) % kQueueBlocks;
			fQueueCount++;
			pthread_cond_signal(&fCond);
		} else {
			fDropped++;
		}
		fAsyncFill = 0;
		fAsyncSlot = -1;
	}
	pthread_mutex_unlock(&fLock);
}

void
RtlDevice::_CaptureLoop()
{
	rtlsdr_dev_t* dev = (rtlsdr_dev_t*)fDevice;
	_ApplyPending();
	rtlsdr_reset_buffer(dev);
	int errors = 0;

	while (!fStopRequested) {
		// macOS can keep libusb transfers queued continuously. The old sync loop
		// left a host-scheduling hole between 6 MB reads; one DAB frame crossed
		// that hole every 1.536 seconds, causing the matching audio tick and
		// frozen video. The callback only copies into the existing block pool.
		_ApplyPending();
		pthread_mutex_lock(&fLock);
		fAsyncFill = 0;
		fAsyncSlot = -1;
		if (fStopRequested) {
			pthread_mutex_unlock(&fLock);
			break;
		}
		size_t blockBytes = fBlockBytes;
		fAsyncActive = false;
		fCancelIssued = false;
		pthread_mutex_unlock(&fLock);

		// Repeatedly cancelling macOS async transfers is not safe even after a
		// drain delay: the Darwin hotplug run loop has been observed calling
		// darwin_async_io_callback on an already-freed transfer. Analog blocks
		// are short enough for synchronous reads; controls are then applied at
		// the next loop boundary without cancellation or callback lifetime races.
		if (blockBytes < kWideBlockBytes) {
			pthread_mutex_lock(&fLock);
			fAsyncActive = false;
			fCancelIssued = false;
			pthread_mutex_unlock(&fLock);
			int got = 0;
			int err = rtlsdr_read_sync(dev, &fScratch[0], (int)blockBytes,
				&got);
			if (err >= 0 && got > 0) {
				_ConsumeAsync(&fScratch[0], (size_t)got);
				errors = 0;
				continue;
			}
			if (fStopRequested)
				break;
			if (++errors >= 25) {
				fLost = true;
				return;
			}
			usleep(20000);
			continue;
		}

		// Atomically enter the async run. A control queued just before this point
		// must be applied first; otherwise nobody would cancel the run because it
		// was not active when the request arrived.
		pthread_mutex_lock(&fLock);
		if (fStopRequested) {
			pthread_mutex_unlock(&fLock);
			break;
		}
		if (fRateDirty || fFreqDirty || fGainDirty || fPpmDirty) {
			pthread_mutex_unlock(&fLock);
			continue;
		}
		fAsyncActive = true;
		fCancelIssued = false;
		pthread_mutex_unlock(&fLock);

		uint32 transferBytes = (uint32)std::min(blockBytes, (size_t)262144);
		int err = rtlsdr_read_async(dev, &RtlDevice::_AsyncCallback, this,
			0, transferBytes);
		pthread_mutex_lock(&fLock);
		fAsyncActive = false;
		fCancelIssued = false;
		pthread_mutex_unlock(&fLock);
		if (fStopRequested)
			break;

		bool controlsPending;
		pthread_mutex_lock(&fLock);
		controlsPending = fRateDirty || fFreqDirty || fGainDirty || fPpmDirty;
		pthread_mutex_unlock(&fLock);
		if (controlsPending) {
			// rtlsdr_read_async() may return before macOS libusb's separate
			// hotplug run loop has delivered the final completion.  Starting a
			// control transfer immediately then races that stale transfer; Air
			// monitor exposed it reliably while retuning every quiet dwell in
			// rtlsdr_set_center_freq.  Stop() already uses this proven drain time.
			usleep(250000);
			errors = 0;
			continue;
		}
		if (err < 0) {
			// A brief stumble is worth retrying; a dongle that has been
			// unplugged never recovers, and retrying forever is what made the
			// app appear to hang - Stop() would then block the UI thread
			// joining a capture thread stuck in libusb.
			if (++errors >= 25) {
				fLost = true;
				return;
			}
			rtlsdr_reset_buffer(dev);
			usleep(20000);
			continue;
		}
		errors = 0;
	}
	pthread_mutex_lock(&fLock);
	fAsyncActive = false;
	fCancelIssued = false;
	pthread_mutex_unlock(&fLock);
}

bool
RtlDevice::NextBlock(std::vector<uint8>& out, uint32& generation,
	bigtime_t timeoutUs)
{
	pthread_mutex_lock(&fLock);
	if (fQueueCount <= 0) {
		struct timespec ts;
		bigtime_t deadline = system_time() + timeoutUs;
		ts.tv_sec = (time_t)(deadline / 1000000);
		ts.tv_nsec = (long)((deadline % 1000000) * 1000);
		// system_time() is gettimeofday-based, so this deadline is directly
		// comparable to CLOCK_REALTIME, which pthread_cond_timedwait uses.
		pthread_cond_timedwait(&fCond, &fLock, &ts);
	}
	if (fQueueCount <= 0) {
		pthread_mutex_unlock(&fLock);
		return false;
	}
	int slot = fReadIndex;
	size_t length = fLengths[slot];
	const uint8* src = &fPool[(size_t)slot * fBlockBytes];
	out.assign(src, src + length);
	generation = fGenerations[slot];
	fReadIndex = (fReadIndex + 1) % kQueueBlocks;
	fQueueCount--;
	pthread_mutex_unlock(&fLock);
	return true;
}
