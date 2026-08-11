#ifndef RSDR_DMB_VIDEO_VIEW_H
#define RSDR_DMB_VIDEO_VIEW_H

#include <Locker.h>
#include <View.h>

#include <vector>

#include "DmbAudio.h"

class BBitmap;

// Software H.264 presentation for Haiku.  The receiver produces bursts of
// length-prefixed access units; a private worker decodes them so the BeAPI UI
// and, more importantly, the USB capture thread never wait for FFmpeg.
class DmbVideoView : public BView {
public:
				DmbVideoView(const char* name);
	virtual		~DmbVideoView();

	virtual void	AttachedToWindow();
	virtual void	Draw(BRect updateRect);
	virtual BSize	MinSize();
	virtual BSize	PreferredSize();

	void		SetConfig(const std::vector<uint8>& avcc);
	void		PushUnits(std::vector<DmbAudio::video_unit>& units);
	void		Reset();
	uint64		Frames() const;

private:
	static status_t	_DecodeEntry(void* cookie);
	void			_DecodeLoop();

	mutable BLocker	fLock;
	std::vector<uint8> fConfig;
	std::vector<DmbAudio::video_unit> fUnits;
	BBitmap*		fBitmap;
	sem_id			fWake;
	thread_id		fThread;
	bool			fStopping;
	bool			fConfigDirty;
	uint32			fGeneration;
	uint64			fFrames;
	uint64			fDecodedFrames;
};

#endif
