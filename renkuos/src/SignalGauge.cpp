#include "SignalGauge.h"

#include <cmath>
#include <cstdio>

#include "Localize.h"

SignalGauge::SignalGauge(const char* name)
	:
	BView(name, B_WILL_DRAW | B_FRAME_EVENTS),
	fQuality(-1.0f),
	fRfDb(-120.0f)
{
	SetViewColor(B_TRANSPARENT_COLOR);
}

void
SignalGauge::SetQuality(float quality, float rfDb)
{
	if (quality > 1.0f)
		quality = 1.0f;
	// Redraw only on a visible change to avoid churning the single-core CPU.
	if (fabsf(quality - fQuality) < 0.02f && fabsf(rfDb - fRfDb) < 1.0f)
		return;
	fQuality = quality;
	fRfDb = rfDb;
	Invalidate();
}

BSize
SignalGauge::MinSize()
{
	return BSize(200, 34);
}

BSize
SignalGauge::PreferredSize()
{
	return BSize(400, 40);
}

BSize
SignalGauge::MaxSize()
{
	return BSize(B_SIZE_UNLIMITED, 44);
}

void
SignalGauge::Draw(BRect)
{
	BRect b = Bounds();
	rgb_color panel = ui_color(B_PANEL_BACKGROUND_COLOR);
	SetHighColor(panel);
	FillRect(b);

	// "Signal" label on the left, vertically centred.
	SetHighColor(ui_color(B_PANEL_TEXT_COLOR));
	SetFont(be_plain_font);
	float mid = (b.top + b.bottom) / 2 + 4;
	const char* label = Tr("Signal");
	DrawString(label, BPoint(b.left + 4, mid));
	float labelW = StringWidth(label) + 12;

	// Reserve room on the right for the "NN%  RF -N dB" readout.
	char text[40];
	if (fQuality >= 0.0f)
		snprintf(text, sizeof(text), "%d%%   RF %.0f dB",
			(int)(fQuality * 100.0f + 0.5f), fRfDb);
	else
		snprintf(text, sizeof(text), "no lock   RF %.0f dB", fRfDb);
	float textW = StringWidth(text) + 10;

	// Horizontal bar track between the label and the readout.
	BRect track(b.left + labelW, b.top + 8, b.right - textW, b.bottom - 8);
	if (track.Width() > 12) {
		SetHighColor(20, 20, 20);
		FillRect(track);
		SetHighColor(90, 90, 90);
		StrokeRect(track);
		if (fQuality > 0.0f) {
			float q = fQuality > 1.0f ? 1.0f : fQuality;
			BRect fill(track.left + 1, track.top + 1,
				track.left + 1 + (track.Width() - 2) * q, track.bottom - 1);
			// Red (bad) -> yellow -> green (good).
			uint8 r = (uint8)(q < 0.5f ? 230 : 230 * (1.0f - q) * 2.0f);
			uint8 g = (uint8)(q < 0.5f ? 230 * q * 2.0f : 210);
			SetHighColor(r, g, 40);
			FillRect(fill);
		}
	}

	SetHighColor(ui_color(B_PANEL_TEXT_COLOR));
	DrawString(text, BPoint(b.right - textW + 8, mid));
}
