/*
 * A vertical signal-quality bar for T-DMB. The value is the smoothed MSC RS
 * success rate (0-1) - the metric that actually predicts whether audio decodes,
 * unlike raw RF level, which stays high through a fade that still kills the MSC.
 */
#ifndef RSDR_SIGNAL_GAUGE_H
#define RSDR_SIGNAL_GAUGE_H

#include <View.h>

class SignalGauge : public BView {
public:
						SignalGauge(const char* name);

			// quality: 0-1, or negative for "no signal yet". rfDb is shown as a
			// secondary readout.
			void		SetQuality(float quality, float rfDb);

	virtual	void		Draw(BRect updateRect);
	virtual	BSize		MinSize();
	virtual	BSize		MaxSize();
	virtual	BSize		PreferredSize();

private:
			float		fQuality;
			float		fRfDb;
};

#endif
