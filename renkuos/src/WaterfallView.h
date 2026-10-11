#ifndef RSDR_WATERFALL_VIEW_H
#define RSDR_WATERFALL_VIEW_H

#include <View.h>

#include <vector>

#include "Demodulator.h"

// Scrolling spectrogram: one pixel row per update, newest at the top.
//
// The history lives in a BBitmap that is scrolled down by one row per line
// rather than being redrawn from a stored history, so the per-frame cost is
// one memmove plus one row of pixels regardless of how deep the history is.
class WaterfallView : public BView {
public:
							WaterfallView(const char* name);
	virtual					~WaterfallView();

	virtual	void			Draw(BRect updateRect);
	virtual	void			AttachedToWindow();
	virtual	void			FrameResized(float width, float height);
	virtual	void			MouseDown(BPoint where);
	virtual	void			MouseMoved(BPoint where, uint32 transit,
								const BMessage* dragMessage);
	virtual	BSize			MinSize();
	virtual	BSize			PreferredSize();

	// floorDb/ceilDb come from the spectrum view so both displays share one
	// auto-ranged scale; without that the waterfall drifts to all-black or
	// all-white as the gain changes.
			void			AddLine(const std::vector<float>& magsDb,
								float floorDb, float ceilDb);
			void			SetTuning(uint64 centerHz, uint32 spanHz,
								demod_mode mode);
			void			Clear();

private:
			void			_EnsureBitmap();

			BBitmap*		fBitmap;
			int				fWidth;
			int				fRows;
			bool			fHasData;
			uint64			fCenterHz;
			uint32			fSpanHz;
			demod_mode		fMode;
			bool			fHovering;
			BPoint			fHoverPoint;
};

#endif // RSDR_WATERFALL_VIEW_H
