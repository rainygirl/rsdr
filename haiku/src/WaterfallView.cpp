#include "WaterfallView.h"

#include <Bitmap.h>
#include <Message.h>
#include <Window.h>

#include "SpectrumView.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// Navy - blue - cyan - yellow - red - white, the ramp SDR receivers have used
// since the beginning. It is not arbitrary: a plain black-to-white ramp puts
// most of the useful range in greys that are indistinguishable on a cheap
// panel, whereas moving through hue as well as brightness keeps every part of
// the range readable.
const int kStops = 6;
const float kRamp[kStops][3] = {
	{ 0.00f, 0.00f, 0.16f },	// noise floor
	{ 0.00f, 0.05f, 0.70f },
	{ 0.00f, 0.75f, 0.85f },
	{ 1.00f, 1.00f, 0.15f },
	{ 1.00f, 0.20f, 0.00f },
	{ 1.00f, 1.00f, 1.00f }		// clipping
};

void
Colour(float t, uint8* bgra)
{
	if (t < 0.0f)
		t = 0.0f;
	if (t > 1.0f)
		t = 1.0f;

	float scaled = t * (float)(kStops - 1);
	int index = (int)scaled;
	if (index >= kStops - 1)
		index = kStops - 2;
	float u = scaled - (float)index;

	float r = kRamp[index][0] + u * (kRamp[index + 1][0] - kRamp[index][0]);
	float g = kRamp[index][1] + u * (kRamp[index + 1][1] - kRamp[index][1]);
	float b = kRamp[index][2] + u * (kRamp[index + 1][2] - kRamp[index][2]);

	// BGRA in memory for B_RGB32 on a little-endian host.
	bgra[0] = (uint8)(b * 255.0f);
	bgra[1] = (uint8)(g * 255.0f);
	bgra[2] = (uint8)(r * 255.0f);
	bgra[3] = 255;
}

} // namespace

WaterfallView::WaterfallView(const char* name)
	:
	BView(name, B_WILL_DRAW | B_FRAME_EVENTS | B_SUPPORTS_LAYOUT),
	fBitmap(NULL),
	fWidth(0),
	fRows(0),
	fHasData(false),
	fCenterHz(0),
	fSpanHz(0),
	fMode(kModeWFM),
	fHovering(false),
	fHoverPoint(0, 0)
{
}

WaterfallView::~WaterfallView()
{
	delete fBitmap;
}

void
WaterfallView::AttachedToWindow()
{
	SetViewColor(B_TRANSPARENT_COLOR);
	BView::AttachedToWindow();
}

BSize
WaterfallView::MinSize()
{
	return BSize(200, 60);
}

BSize
WaterfallView::PreferredSize()
{
	return BSize(600, 120);
}

void
WaterfallView::FrameResized(float width, float height)
{
	BView::FrameResized(width, height);
	_EnsureBitmap();
	Invalidate();
}

void
WaterfallView::_EnsureBitmap()
{
	// The bitmap covers only the plot area, inset by the same amount as the
	// spectrum's dB axis above, so a feature at a given x is at the same
	// frequency in both views.
	BRect bounds = Bounds();
	int width = (int)(bounds.Width() - SpectrumView::AxisWidth());
	int rows = (int)bounds.Height();
	if (width < 2 || rows < 2)
		return;

	if (fBitmap != NULL && width == fWidth && rows == fRows)
		return;

	BBitmap* old = fBitmap;
	int oldWidth = fWidth;
	int oldRows = fRows;
	bool hadData = fHasData;
	fWidth = width;
	fRows = rows;
	fBitmap = new BBitmap(BRect(0, 0, width - 1, rows - 1), B_RGB32, false);
	if (fBitmap->InitCheck() != B_OK || fBitmap->Bits() == NULL) {
		delete fBitmap;
		fBitmap = NULL;
		delete old;
		return;
	}

	// Fill with the palette's own floor colour rather than black. The
	// waterfall only gains 15 rows a second, so a fresh one spends several
	// seconds mostly empty; filling it with the bottom of the scale makes
	// that read as "no history yet" instead of "broken".
	uint8* bits = (uint8*)fBitmap->Bits();
	const int32 bpr = fBitmap->BytesPerRow();
	uint8 floorColour[4];
	Colour(0.0f, floorColour);
	for (int y = 0; y < rows; y++) {
		uint8* row = bits + y * bpr;
		for (int x = 0; x < width; x++)
			memcpy(row + x * 4, floorColour, 4);
	}

	// Preserve the existing time history across window resizing. Frequency is
	// resampled horizontally with nearest-neighbour pixels; rows keep their
	// age, newest at the top. The former delete-and-wait-for-new-data path made
	// the waterfall flash and appear to restart while the layout was settling.
	if (old != NULL && old->Bits() != NULL && oldWidth > 0 && oldRows > 0
			&& hadData) {
		const uint8* oldBits = (const uint8*)old->Bits();
		int32 oldBpr = old->BytesPerRow();
		int copyRows = oldRows < rows ? oldRows : rows;
		for (int y = 0; y < copyRows; y++) {
			uint8* row = bits + y * bpr;
			const uint8* oldRow = oldBits + y * oldBpr;
			for (int x = 0; x < width; x++) {
				int oldX = (int)((int64)x * oldWidth / width);
				if (oldX >= oldWidth)
					oldX = oldWidth - 1;
				memcpy(row + x * 4, oldRow + oldX * 4, 4);
			}
		}
	}
	delete old;
	fHasData = hadData;
}

void
WaterfallView::SetTuning(uint64 centerHz, uint32 spanHz, demod_mode mode)
{
	fCenterHz = centerHz;
	fSpanHz = spanHz;
	fMode = mode;
	if (fHovering)
		Invalidate();
}

void
WaterfallView::MouseDown(BPoint where)
{
	if (fSpanHz == 0 || (fMode != kModeAM && fMode != kModeWFM
			&& fMode != kModeAir))
		return;
	BRect plot = Bounds();
	plot.left += SpectrumView::AxisWidth();
	if (!plot.Contains(where) || plot.Width() <= 0)
		return;
	float fraction = (where.x - plot.left) / plot.Width() - 0.5f;
	BMessage message(SpectrumView::kMsgSpectrumClicked);
	message.AddFloat("offsetHz", fraction * (float)fSpanHz);
	if (Window() != NULL)
		Window()->PostMessage(&message);
}

void
WaterfallView::MouseMoved(BPoint where, uint32 transit,
	const BMessage* dragMessage)
{
	(void)dragMessage;
	BRect plot = Bounds();
	plot.left += SpectrumView::AxisWidth();
	bool usefulMode = fMode == kModeAM || fMode == kModeWFM
		|| fMode == kModeAir;
	bool hovering = transit != B_EXITED_VIEW && plot.Contains(where)
		&& fSpanHz > 0 && usefulMode;
	if (hovering != fHovering || (hovering && where != fHoverPoint)) {
		fHovering = hovering;
		fHoverPoint = where;
		Invalidate();
	}
}

void
WaterfallView::Clear()
{
	if (fBitmap != NULL)
		memset(fBitmap->Bits(), 0, fBitmap->BitsLength());
	fHasData = false;
	Invalidate();
}

void
WaterfallView::AddLine(const std::vector<float>& magsDb, float floorDb,
	float ceilDb)
{
	if (magsDb.empty())
		return;

	_EnsureBitmap();
	if (fBitmap == NULL)
		return;

	uint8* bits = (uint8*)fBitmap->Bits();
	const int32 bpr = fBitmap->BytesPerRow();

	// Scroll down one row, oldest line falls off the bottom.
	memmove(bits + bpr, bits, (size_t)bpr * (size_t)(fRows - 1));

	const float range = ceilDb - floorDb > 1.0f ? ceilDb - floorDb : 1.0f;
	const size_t bins = magsDb.size();

	for (int x = 0; x < fWidth; x++) {
		size_t begin = bins * (size_t)x / (size_t)fWidth;
		size_t end = bins * (size_t)(x + 1) / (size_t)fWidth;
		if (end <= begin)
			end = begin + 1;
		if (end > bins)
			end = bins;

		// Peak within the column, matching the spectrum trace above it -
		// averaging would hide narrow carriers, which are the whole point.
		float peak = magsDb[begin];
		for (size_t i = begin + 1; i < end; i++) {
			if (magsDb[i] > peak)
				peak = magsDb[i];
		}

		Colour((peak - floorDb) / range, bits + x * 4);
	}

	fHasData = true;
	Invalidate();
}

void
WaterfallView::Draw(BRect updateRect)
{
	SetHighColor(6, 8, 24);
	FillRect(Bounds());
	if (fBitmap == NULL || !fHasData)
		return;
	DrawBitmap(fBitmap, BPoint(Bounds().left + SpectrumView::AxisWidth(),
		Bounds().top));

	if (fHovering && fSpanHz > 0) {
		BRect plot = Bounds();
		plot.left += SpectrumView::AxisWidth();
		double fraction = (double)(fHoverPoint.x - plot.left)
			/ (double)plot.Width() - 0.5;
		double hz = (double)fCenterHz + fraction * (double)fSpanHz;
		char value[32];
		if (fMode == kModeAM)
			snprintf(value, sizeof(value), "%.1f kHz", hz / 1e3);
		else
			snprintf(value, sizeof(value), "%.3f MHz", hz / 1e6);
		SetFontSize(10.0f);
		float width = StringWidth(value) + 10.0f;
		float x = fHoverPoint.x + 12.0f;
		if (x + width > Bounds().right)
			x = fHoverPoint.x - width - 8.0f;
		float y = fHoverPoint.y - 20.0f;
		if (y < Bounds().top)
			y = fHoverPoint.y + 8.0f;
		BRect tip(x, y, x + width, y + 18.0f);
		SetHighColor(12, 16, 30, 230);
		FillRoundRect(tip, 4.0f, 4.0f);
		SetHighColor(238, 242, 255);
		DrawString(value, BPoint(tip.left + 5.0f, tip.bottom - 5.0f));
	}
}
