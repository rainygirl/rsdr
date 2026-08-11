#include "SpectrumView.h"

#include <Message.h>
#include <Window.h>

#include <cmath>
#include <cstdio>
#include <cstring>

SpectrumView::SpectrumView(const char* name)
	:
	BView(name, B_WILL_DRAW | B_FRAME_EVENTS | B_SUPPORTS_LAYOUT),
	fSpanHz(0),
	fCenterHz(0),
	fFineTuneHz(0.0f),
	fChannelBandwidth(0),
	fMode(kModeWFM),
	fFloorDb(-100.0f),
	fCeilDb(-10.0f)
{
}

SpectrumView::~SpectrumView()
{
}

void
SpectrumView::AttachedToWindow()
{
	SetViewColor(B_TRANSPARENT_COLOR);
	SetLowColor(6, 8, 24);
	SetFontSize(9.0f);
	BView::AttachedToWindow();
}

BSize
SpectrumView::MinSize()
{
	return BSize(240, 110);
}

BSize
SpectrumView::PreferredSize()
{
	return BSize(640, 190);
}

BRect
SpectrumView::_PlotRect() const
{
	BRect bounds = Bounds();
	return BRect(bounds.left + AxisWidth(), bounds.top,
		bounds.right, bounds.bottom - LabelHeight());
}

void
SpectrumView::SetData(const std::vector<float>& magsDb, uint32 spanHz)
{
	fMagsDb = magsDb;
	fSpanHz = spanHz;

	if (!fMagsDb.empty()) {
		// Auto-range from the data: the useful dynamic range moves with the
		// gain setting and with how strong the local signals are, and a fixed
		// scale is either all noise or all clipping.
		float hi = fMagsDb[0];
		double sum = 0.0;
		for (size_t i = 0; i < fMagsDb.size(); i++) {
			if (fMagsDb[i] > hi)
				hi = fMagsDb[i];
			sum += fMagsDb[i];
		}
		float mean = (float)(sum / (double)fMagsDb.size());
		// A little below the average bin (which is dominated by noise) and a
		// little above the strongest.
		float targetFloor = mean - 12.0f;
		float targetCeil = hi + 6.0f;
		if (targetCeil - targetFloor < 40.0f)
			targetCeil = targetFloor + 40.0f;
		// Smoothed, so the trace does not jump vertically every frame.
		fFloorDb += 0.2f * (targetFloor - fFloorDb);
		fCeilDb += 0.2f * (targetCeil - fCeilDb);
	}

	Invalidate();
}

void
SpectrumView::SetTuning(uint64 centerHz, float fineTuneHz,
	uint32 channelBandwidthHz, demod_mode mode)
{
	fCenterHz = centerHz;
	fFineTuneHz = fineTuneHz;
	fChannelBandwidth = channelBandwidthHz;
	fMode = mode;
}

float
SpectrumView::_OffsetToX(float offsetHz) const
{
	BRect plot = _PlotRect();
	if (fSpanHz == 0)
		return plot.left;
	float frac = 0.5f + offsetHz / (float)fSpanHz;
	return plot.left + frac * plot.Width();
}

void
SpectrumView::MouseDown(BPoint where)
{
	BRect plot = _PlotRect();
	if (fSpanHz == 0 || plot.Width() <= 0)
		return;

	float frac = (where.x - plot.left) / plot.Width() - 0.5f;
	float offsetHz = frac * (float)fSpanHz;

	BMessage msg(kMsgSpectrumClicked);
	msg.AddFloat("offsetHz", offsetHz);
	if (Window() != NULL)
		Window()->PostMessage(&msg);
}

void
SpectrumView::Draw(BRect updateRect)
{
	BRect bounds = Bounds();
	BRect plot = _PlotRect();

	SetHighColor(6, 8, 24);
	FillRect(bounds);
	SetLowColor(6, 8, 24);

	if (fMagsDb.empty() || fSpanHz == 0) {
		SetHighColor(110, 110, 130);
		DrawString("no signal data", BPoint(plot.left + 6, plot.top + 16));
		return;
	}

	const float range = fCeilDb - fFloorDb;

	// Channel passband behind everything else.
	if (fChannelBandwidth > 0) {
		float half = (float)fChannelBandwidth / 2.0f;
		float x0 = _OffsetToX(fFineTuneHz - half);
		float x1 = _OffsetToX(fFineTuneHz + half);
		if (x0 < plot.left)
			x0 = plot.left;
		if (x1 > plot.right)
			x1 = plot.right;
		SetHighColor(24, 40, 78);
		FillRect(BRect(x0, plot.top, x1, plot.bottom));
	}

	// Horizontal graticule and dB labels, on whole 10 dB steps.
	SetFontSize(9.0f);
	int step = 10;
	while (range / (float)step > 8.0f)
		step += 10;
	int first = (int)(ceilf(fFloorDb / (float)step) * (float)step);
	for (int db = first; (float)db <= fCeilDb; db += step) {
		float y = plot.bottom - ((float)db - fFloorDb) / range * plot.Height();
		SetHighColor(34, 40, 66);
		StrokeLine(BPoint(plot.left, y), BPoint(plot.right, y));

		char label[16];
		snprintf(label, sizeof(label), "%d", db);
		SetHighColor(130, 140, 170);
		float w = StringWidth(label);
		DrawString(label, BPoint(plot.left - 4.0f - w, y + 3.0f));
	}

	// Vertical graticule and frequency labels. Divisions land on round
	// fractions of the span so the numbers stay readable.
	const int kDivisions = 8;
	for (int i = 0; i <= kDivisions; i++) {
		float frac = (float)i / (float)kDivisions;
		float x = plot.left + frac * plot.Width();
		if (i > 0 && i < kDivisions) {
			SetHighColor(34, 40, 66);
			StrokeLine(BPoint(x, plot.top), BPoint(x, plot.bottom));
		}

		double hz = (double)fCenterHz + ((double)frac - 0.5) * (double)fSpanHz;
		char label[24];
		if (fMode == kModeAM)
			snprintf(label, sizeof(label), "%.1f", hz / 1e3);
		else
			snprintf(label, sizeof(label), "%.3f", hz / 1e6);
		SetHighColor(130, 140, 170);
		float w = StringWidth(label);
		float lx = x - w / 2.0f;
		if (lx < bounds.left)
			lx = bounds.left;
		if (lx + w > bounds.right)
			lx = bounds.right - w;
		DrawString(label, BPoint(lx, bounds.bottom - 4.0f));
	}

	// The trace, filled underneath: one column per pixel, peak-held within the
	// column (averaging hides narrow carriers, which are what one is looking
	// for).
	//
	// Drawn as two polygons rather than a StrokeLine per column. That is not a
	// micro-optimisation: every drawing call is a message to the app_server,
	// and at 700 columns x 2 lines x 15 frames a second the per-column version
	// was issuing over twenty thousand of them per second. On a single-core
	// machine that starved the demodulator thread badly enough to be audible
	// as a tick about once a second. This is two calls per frame.
	const int columns = (int)plot.Width();
	const size_t bins = fMagsDb.size();
	if (columns < 2 || bins == 0)
		return;

	fTracePoints.resize((size_t)columns);
	for (int x = 0; x < columns; x++) {
		size_t begin = bins * (size_t)x / (size_t)columns;
		size_t end = bins * (size_t)(x + 1) / (size_t)columns;
		if (end <= begin)
			end = begin + 1;
		if (end > bins)
			end = bins;

		float peak = fMagsDb[begin];
		for (size_t i = begin + 1; i < end; i++) {
			if (fMagsDb[i] > peak)
				peak = fMagsDb[i];
		}

		float norm = (peak - fFloorDb) / (range > 0.0f ? range : 1.0f);
		if (norm < 0.0f)
			norm = 0.0f;
		if (norm > 1.0f)
			norm = 1.0f;
		fTracePoints[x].x = plot.left + (float)x;
		fTracePoints[x].y = plot.bottom - norm * plot.Height();
	}

	// Fill: the trace plus the two bottom corners, closed.
	fFillPoints.resize(fTracePoints.size() + 2);
	memcpy(&fFillPoints[0], &fTracePoints[0],
		fTracePoints.size() * sizeof(BPoint));
	fFillPoints[fTracePoints.size()] = BPoint(plot.right, plot.bottom);
	fFillPoints[fTracePoints.size() + 1] = BPoint(plot.left, plot.bottom);

	SetHighColor(30, 74, 150);
	FillPolygon(&fFillPoints[0], (int32)fFillPoints.size());

	SetHighColor(210, 230, 255);
	StrokePolygon(&fTracePoints[0], (int32)fTracePoints.size(), false);

	// Tuned frequency, and where the demodulator is actually listening once
	// fine tuning is applied.
	SetHighColor(230, 70, 70);
	float cx = _OffsetToX(0.0f);
	StrokeLine(BPoint(cx, plot.top), BPoint(cx, plot.bottom));

	if (fFineTuneHz != 0.0f) {
		SetHighColor(255, 190, 60);
		float fx = _OffsetToX(fFineTuneHz);
		StrokeLine(BPoint(fx, plot.top), BPoint(fx, plot.bottom));
	}
}
