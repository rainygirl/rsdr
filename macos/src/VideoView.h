/*
 * H.264 display for T-DMB video, on AVSampleBufferDisplayLayer.
 *
 * Nothing here decodes anything. The access units arrive already in the shape
 * Core Media wants - length-prefixed NAL units, exactly as the avcC record
 * from the object descriptor stream describes them - so the whole job is to
 * build a format description from the parameter sets, wrap each access unit in
 * a CMSampleBuffer and hand it over. The layer owns the decoder and the
 * display timing.
 *
 * PES timestamps are retained and compared with the audio sink's playback PTS.
 * The view still feeds frames itself rather than making AVSampleBufferDisplayLayer
 * wait on a discontinuous stream clock: early frames wait, overdue frames catch
 * up in decode order, and untimed damaged input falls back to 30 fps.
 */
#ifndef RSDR_MACOS_VIDEO_VIEW_H
#define RSDR_MACOS_VIDEO_VIEW_H

#import <Cocoa/Cocoa.h>

#include <vector>

#include "SupportDefs.h"

@interface VideoView : NSView

// The avcC record. Passing a different one rebuilds the format description.
- (void)setConfig:(const std::vector<uint8>&)avcC;
// One access unit, length-prefixed as the avcC record specifies. PTS is the
// MPEG 90 kHz clock; untimed units retain the old 30 fps fallback.
- (void)pushUnit:(const std::vector<uint8>&)au pts:(uint64)pts90k
		hasPts:(BOOL)hasPts;
- (void)setAudioPts:(uint64)pts90k valid:(BOOL)valid;
- (void)setWideAspect:(BOOL)wide;
- (void)clearPicture;
- (int)framesShown;

@end

#endif // RSDR_MACOS_VIDEO_VIEW_H
