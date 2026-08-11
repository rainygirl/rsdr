#import "VideoView.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>

namespace {
struct PendingVideoFrame {
	std::vector<uint8> data;
	uint64 pts90k;
	bool hasPts;
	PendingVideoFrame() : pts90k(0), hasPts(false) {}
};

int64
PtsDifference(uint64 a, uint64 b)
{
	const uint64 mask = 0x1ffffffffULL;
	int64 d = (int64)((a - b) & mask);
	if (d >= (1LL << 32))
		d -= (1LL << 33);
	return d;
}
}

@implementation VideoView {
	AVSampleBufferDisplayLayer*	_layer;
	CMVideoFormatDescriptionRef	_format;
	std::vector<uint8>			_config;
	int							_frames;
	int							_nalLength;
	std::vector<PendingVideoFrame> _pending;
	NSTimer*					_frameTimer;
	BOOL						_wideAspect;
	uint64						_audioPts90k;
	CFAbsoluteTime				_audioClockWall;
	BOOL						_haveAudioClock;
}

- (instancetype)initWithFrame:(NSRect)frame
{
	self = [super initWithFrame:frame];
	if (self == nil)
		return nil;

	[self setWantsLayer:YES];
	_layer = [[AVSampleBufferDisplayLayer alloc] init];
	[_layer setVideoGravity:AVLayerVideoGravityResizeAspect];
	[_layer setBackgroundColor:CGColorCreateGenericGray(0.07, 1.0)];
	[_layer setFrame:[self bounds]];
	[[self layer] addSublayer:_layer];
	[[self layer] setBackgroundColor:CGColorCreateGenericGray(0.07, 1.0)];

	_format = NULL;
	_frames = 0;
	_nalLength = 4;
	_wideAspect = YES;
	_audioPts90k = 0;
	_audioClockWall = 0;
	_haveAudioClock = NO;
	_frameTimer = [NSTimer timerWithTimeInterval:1.0 / 30.0 target:self
		selector:@selector(displayNextFrame:) userInfo:nil repeats:YES];
	[[NSRunLoop mainRunLoop] addTimer:_frameTimer forMode:NSRunLoopCommonModes];
	return self;
}

- (void)dealloc
{
	[_frameTimer invalidate];
	if (_format != NULL)
		CFRelease(_format);
}

- (void)setFrameSize:(NSSize)size
{
	[super setFrameSize:size];
	[self layoutVideoLayer];
}

- (void)layoutVideoLayer
{
	NSRect b = [self bounds];
	CGFloat aspect = _wideAspect ? 16.0 / 9.0 : 4.0 / 3.0;
	CGFloat width = b.size.width;
	CGFloat height = width / aspect;
	if (height > b.size.height) {
		height = b.size.height;
		width = height * aspect;
	}
	[_layer setFrame:NSMakeRect((b.size.width - width) / 2.0,
		(b.size.height - height) / 2.0, width, height)];
}

- (void)setWideAspect:(BOOL)wide
{
	_wideAspect = wide;
	// Resize fills the explicitly shaped 16:9 or 4:3 layer. ResizeAspect would
	// preserve the transmitted 4:3 sample aspect and defeat this user option.
	[_layer setVideoGravity:AVLayerVideoGravityResize];
	[self layoutVideoLayer];
}

- (int)framesShown
{
	return _frames;
}

- (void)clearPicture
{
	_pending.clear();
	[_layer flushAndRemoveImage];
	_frames = 0;
	_haveAudioClock = NO;
}

- (void)setConfig:(const std::vector<uint8>&)avcC
{
	if (avcC.size() < 7)
		return;
	if (avcC == _config)
		return;
	_pending.clear();
	_config = avcC;

	// avcC: version, profile, compat, level, 0xFC|lengthSizeMinusOne,
	// 0xE0|numSPS, then each SPS as a 16-bit length and its bytes, then the
	// PPS count and the PPS entries the same way.
	const uint8* d = &avcC[0];
	size_t n = avcC.size();
	_nalLength = (d[4] & 0x03) + 1;

	std::vector<const uint8*> sets;
	std::vector<size_t> sizes;
	size_t p = 5;
	int numSps = d[p++] & 0x1F;
	for (int i = 0; i < numSps && p + 2 <= n; i++) {
		size_t len = ((size_t)d[p] << 8) | d[p + 1];
		p += 2;
		if (p + len > n)
			return;
		sets.push_back(d + p);
		sizes.push_back(len);
		p += len;
	}
	if (p >= n)
		return;
	int numPps = d[p++];
	for (int i = 0; i < numPps && p + 2 <= n; i++) {
		size_t len = ((size_t)d[p] << 8) | d[p + 1];
		p += 2;
		if (p + len > n)
			return;
		sets.push_back(d + p);
		sizes.push_back(len);
		p += len;
	}
	if (sets.size() < 2)
		return;

	if (_format != NULL) {
		CFRelease(_format);
		_format = NULL;
	}
	OSStatus err = CMVideoFormatDescriptionCreateFromH264ParameterSets(
		kCFAllocatorDefault, sets.size(), sets.data(), sizes.data(),
		_nalLength, &_format);
	if (err != noErr)
		_format = NULL;
}

- (void)pushUnit:(const std::vector<uint8>&)au pts:(uint64)pts90k
		hasPts:(BOOL)hasPts
{
	if (_format == NULL || au.empty())
		return;
	// A normal 6 MB receive block contains about 46 frames. Keep several
	// blocks so a short UI delay does not discard a slice of every burst.
	if (_pending.size() >= 120)
		_pending.erase(_pending.begin());
	PendingVideoFrame frame;
	frame.data = au;
	frame.pts90k = pts90k;
	frame.hasPts = hasPts;
	_pending.push_back(frame);
}

- (void)setAudioPts:(uint64)pts90k valid:(BOOL)valid
{
	_haveAudioClock = valid;
	if (valid) {
		_audioPts90k = pts90k;
		_audioClockWall = CFAbsoluteTimeGetCurrent();
	}
}

- (BOOL)displayFrame:(const std::vector<uint8>&)au
{
	if (![_layer isReadyForMoreMediaData])
		return NO;

	CMBlockBufferRef block = NULL;
	if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, NULL,
			au.size(), kCFAllocatorDefault, NULL, 0, au.size(), 0, &block)
			!= kCMBlockBufferNoErr) {
		return NO;
	}
	if (CMBlockBufferReplaceDataBytes(&au[0], block, 0, au.size())
			!= kCMBlockBufferNoErr) {
		CFRelease(block);
		return NO;
	}

	CMSampleBufferRef sample = NULL;
	size_t sampleSize = au.size();
	if (CMSampleBufferCreateReady(kCFAllocatorDefault, block, _format, 1, 0,
			NULL, 1, &sampleSize, &sample) != noErr) {
		CFRelease(block);
		return NO;
	}

	// The timer supplies presentation cadence; DisplayImmediately keeps a lost
	// stream timestamp from stalling the hardware decoder after RF damage.
	CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample,
		YES);
	if (attachments != NULL && CFArrayGetCount(attachments) > 0) {
		CFMutableDictionaryRef dict = (CFMutableDictionaryRef)
			CFArrayGetValueAtIndex(attachments, 0);
		CFDictionarySetValue(dict, kCMSampleAttachmentKey_DisplayImmediately,
			kCFBooleanTrue);
	}

	if ([_layer status] == AVQueuedSampleBufferRenderingStatusFailed)
		[_layer flush];
	[_layer enqueueSampleBuffer:sample];
	_frames++;

	CFRelease(sample);
	CFRelease(block);
	return YES;
}

- (void)displayNextFrame:(NSTimer*)timer
{
	if (_format == NULL || _pending.empty())
		return;

	int budget = 1;
	while (budget-- > 0 && !_pending.empty()) {
		const PendingVideoFrame& frame = _pending.front();
		bool frameHasPts = frame.hasPts;
		int64 difference = 0;
		if (frame.hasPts) {
			// Do not start picture during the audio sink's 2.5-second prime.
			// Once audio starts, extrapolate between the UI's 100 ms updates so
			// the 30 fps timer still has frame-level timing resolution.
			if (!_haveAudioClock)
				return;
			double elapsed = CFAbsoluteTimeGetCurrent() - _audioClockWall;
			uint64 nowPts = (_audioPts90k + (uint64)(elapsed * 90000.0))
				& 0x1ffffffffULL;
			difference = PtsDifference(frame.pts90k, nowPts);
			if (difference > 3000) // more than one 30 fps frame early
				return;
			// Decode a few overdue frames in order so H.264 references remain
			// intact, but let the last one become the visible frame this tick.
			if (difference < -9000 && budget < 3)
				budget = 3;
		}

		std::vector<uint8> data = frame.data;
		if (![self displayFrame:data])
			return;
		_pending.erase(_pending.begin());
		if (!frameHasPts || difference >= -9000)
			break;
	}
}

@end
