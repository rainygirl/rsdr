#ifndef RSDR_SPECTRUM_VIEW_H
#define RSDR_SPECTRUM_VIEW_H

#include <View.h>

#include <vector>

#include "Demodulator.h"

// Power spectrum of the tuner's whole passband, with a dB scale down the left
// edge, frequency labels along the bottom, and the demodulator's channel
// drawn on it. This is what makes fine-tuning SSB possible at all - a 300 Hz
// error is inaudible on a level meter and obvious here.
//
// Clicking anywhere in the plot sends kMsgSpectrumClicked with an "offsetHz"
// float, so a signal can be tuned by pointing at it.
class SpectrumView : public BView {
public:
	static const uint32		kMsgSpectrumClicked = 'spCk';

	// Room for the dB labels and the frequency labels. WaterfallView uses the
	// same left inset so that the two displays line up column for column -
	// a waterfall whose frequency axis is offset from the spectrum above it
	// is worse than no waterfall. (Functions rather than static const floats,
	// which C++11 will not let a class initialise in place.)
	static	float			AxisWidth() { return 34.0f; }
	static	float			LabelHeight() { return 15.0f; }

							SpectrumView(const char* name);
	virtual					~SpectrumView();

	virtual	void			Draw(BRect updateRect);
	virtual	void			AttachedToWindow();
	virtual	void			MouseDown(BPoint where);
	virtual	BSize			MinSize();
	virtual	BSize			PreferredSize();

			void			SetData(const std::vector<float>& magsDb,
								uint32 spanHz);
			void			SetTuning(uint64 centerHz, float fineTuneHz,
								uint32 channelBandwidthHz, demod_mode mode);

	// The auto-ranged scale, so the waterfall can use the same one.
			float			FloorDb() const { return fFloorDb; }
			float			CeilDb() const { return fCeilDb; }

private:
			BRect			_PlotRect() const;
			float			_OffsetToX(float offsetHz) const;

	std::vector<float>		fMagsDb;
	// Reused between frames so a redraw does not allocate.
	std::vector<BPoint>		fTracePoints;
	std::vector<BPoint>		fFillPoints;
			uint32			fSpanHz;
			uint64			fCenterHz;
			float			fFineTuneHz;
			uint32			fChannelBandwidth;
			demod_mode		fMode;
			float			fFloorDb;
			float			fCeilDb;
};

#endif // RSDR_SPECTRUM_VIEW_H
