#include "DmbVideoView.h"

#include <Autolock.h>
#include <Bitmap.h>
#include <Font.h>
#include <OS.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstring>

namespace {

bool
AvccParameterSets(const std::vector<uint8>& config, std::vector<uint8>& out,
	int& nalLength)
{
	out.clear();
	if (config.size() < 7 || config[0] != 1)
		return false;
	nalLength = (config[4] & 3) + 1;
	size_t p = 5;
	int spsCount = config[p++] & 0x1f;
	for (int pass = 0; pass < 2; pass++) {
		int count = pass == 0 ? spsCount : (p < config.size() ? config[p++] : 0);
		for (int i = 0; i < count; i++) {
			if (p + 2 > config.size())
				return false;
			size_t size = ((size_t)config[p] << 8) | config[p + 1];
			p += 2;
			if (p + size > config.size())
				return false;
			static const uint8 start[] = { 0, 0, 0, 1 };
			out.insert(out.end(), start, start + sizeof(start));
			out.insert(out.end(), config.begin() + p, config.begin() + p + size);
			p += size;
		}
	}
	return !out.empty();
}

bool
ToAnnexB(const std::vector<uint8>& input, int nalLength,
	std::vector<uint8>& output)
{
	output.clear();
	size_t p = 0;
	while (p + (size_t)nalLength <= input.size()) {
		size_t size = 0;
		for (int i = 0; i < nalLength; i++)
			size = (size << 8) | input[p++];
		if (size == 0 || p + size > input.size())
			return false;
		static const uint8 start[] = { 0, 0, 0, 1 };
		output.insert(output.end(), start, start + sizeof(start));
		output.insert(output.end(), input.begin() + p, input.begin() + p + size);
		p += size;
	}
	return p == input.size() && !output.empty();
}

} // namespace

DmbVideoView::DmbVideoView(const char* name)
	:
	BView(name, B_WILL_DRAW | B_FRAME_EVENTS | B_SUPPORTS_LAYOUT),
	fLock("DMB video"),
	fBitmap(NULL),
	fWake(create_sem(0, "DMB video frames")),
	fThread(-1),
	fStopping(false),
	fConfigDirty(false),
	fGeneration(1),
	fFrames(0),
	fDecodedFrames(0)
{
	fThread = spawn_thread(&_DecodeEntry, "DMB H.264 decoder",
		B_LOW_PRIORITY, this);
	if (fThread >= 0)
		resume_thread(fThread);
}

DmbVideoView::~DmbVideoView()
{
	{
		BAutolock lock(fLock);
		fStopping = true;
	}
	if (fWake >= 0)
		release_sem(fWake);
	if (fThread >= 0) {
		status_t result;
		wait_for_thread(fThread, &result);
	}
	if (fWake >= 0)
		delete_sem(fWake);
	delete fBitmap;
}

void
DmbVideoView::AttachedToWindow()
{
	SetViewColor(0, 0, 0);
	SetLowColor(0, 0, 0);
	SetHighColor(205, 205, 205);
	BView::AttachedToWindow();
}

BSize
DmbVideoView::MinSize()
{
	return BSize(240, 170);
}

BSize
DmbVideoView::PreferredSize()
{
	return BSize(640, 360);
}

void
DmbVideoView::Draw(BRect updateRect)
{
	FillRect(updateRect, B_SOLID_LOW);
	BAutolock lock(fLock);
	if (fBitmap == NULL) {
		const char* text = "Waiting for T-DMB signal...";
		float width = StringWidth(text);
		DrawString(text, BPoint((Bounds().Width() - width) / 2,
			Bounds().Height() / 2));
		return;
	}
	BRect source = fBitmap->Bounds();
	float scale = std::min(Bounds().Width() / source.Width(),
		Bounds().Height() / source.Height());
	BRect target(0, 0, source.Width() * scale, source.Height() * scale);
	target.OffsetTo((Bounds().Width() - target.Width()) / 2,
		(Bounds().Height() - target.Height()) / 2);
	DrawBitmap(fBitmap, source, target, B_FILTER_BITMAP_BILINEAR);
}

void
DmbVideoView::SetConfig(const std::vector<uint8>& avcc)
{
	BAutolock lock(fLock);
	if (avcc == fConfig)
		return;
	fConfig = avcc;
	fConfigDirty = true;
	fUnits.clear();
	fGeneration++;
	release_sem(fWake);
}

void
DmbVideoView::PushUnits(std::vector<DmbAudio::video_unit>& units)
{
	if (units.empty())
		return;
	BAutolock lock(fLock);
	fUnits.insert(fUnits.end(), units.begin(), units.end());
	units.clear();
	if (fUnits.size() > 120)
		fUnits.erase(fUnits.begin(), fUnits.begin() + (fUnits.size() - 120));
	release_sem(fWake);
}

void
DmbVideoView::Reset()
{
	BBitmap* old = NULL;
	{
		BAutolock lock(fLock);
		fConfig.clear();
		fUnits.clear();
		fConfigDirty = true;
		fGeneration++;
		fFrames = 0;
		fDecodedFrames = 0;
		old = fBitmap;
		fBitmap = NULL;
	}
	delete old;
	release_sem(fWake);
	Invalidate();
}

uint64
DmbVideoView::Frames() const
{
	BAutolock lock(fLock);
	return fFrames;
}

status_t
DmbVideoView::_DecodeEntry(void* cookie)
{
	static_cast<DmbVideoView*>(cookie)->_DecodeLoop();
	return B_OK;
}

void
DmbVideoView::_DecodeLoop()
{
	AVCodecContext* decoder = NULL;
	AVFrame* frame = av_frame_alloc();
	AVPacket* packet = av_packet_alloc();
	SwsContext* scaler = NULL;
	int nalLength = 4;
	uint32 decoderGeneration = 0;

	while (true) {
		acquire_sem(fWake);
		std::vector<uint8> config;
		std::vector<DmbAudio::video_unit> units;
		uint32 generation;
		bool reconfigure;
		{
			BAutolock lock(fLock);
			if (fStopping)
				break;
			config = fConfig;
			units.swap(fUnits);
			generation = fGeneration;
			reconfigure = fConfigDirty || decoderGeneration != generation;
			fConfigDirty = false;
		}

		if (reconfigure) {
			avcodec_free_context(&decoder);
			sws_freeContext(scaler);
			scaler = NULL;
			decoderGeneration = generation;
			std::vector<uint8> extra;
			if (!AvccParameterSets(config, extra, nalLength))
				continue;
			const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
			if (codec == NULL)
				continue;
			decoder = avcodec_alloc_context3(codec);
			if (decoder == NULL)
				continue;
			decoder->thread_count = 1;
			decoder->flags2 |= AV_CODEC_FLAG2_FAST;
			decoder->extradata_size = (int)extra.size();
			decoder->extradata = (uint8_t*)av_mallocz(extra.size()
				+ AV_INPUT_BUFFER_PADDING_SIZE);
			if (decoder->extradata == NULL) {
				avcodec_free_context(&decoder);
				continue;
			}
			memcpy(decoder->extradata, &extra[0], extra.size());
			if (avcodec_open2(decoder, codec, NULL) < 0) {
				avcodec_free_context(&decoder);
				continue;
			}
		}
		if (decoder == NULL)
			continue;

		for (size_t i = 0; i < units.size(); i++) {
			std::vector<uint8> annexb;
			if (!ToAnnexB(units[i].data, nalLength, annexb))
				continue;
			packet->data = &annexb[0];
			packet->size = (int)annexb.size();
			if (avcodec_send_packet(decoder, packet) < 0)
				continue;
			while (avcodec_receive_frame(decoder, frame) == 0) {
				fDecodedFrames++;
				// Keep all decoded reference frames for H.264 correctness, but
				// publish only one in twenty to keep BeOS redraw overhead small.
				if ((fDecodedFrames % 20) != 0)
					continue;
				if (frame->width <= 0 || frame->height <= 0)
					continue;
				BBitmap* bitmap = new BBitmap(BRect(0, 0, frame->width - 1,
					frame->height - 1), B_RGB32);
				if (bitmap->InitCheck() != B_OK || bitmap->Bits() == NULL) {
					delete bitmap;
					continue;
				}
				scaler = sws_getCachedContext(scaler, frame->width, frame->height,
					(AVPixelFormat)frame->format, frame->width, frame->height,
					AV_PIX_FMT_BGRA, SWS_FAST_BILINEAR, NULL, NULL, NULL);
				if (scaler == NULL) {
					delete bitmap;
					continue;
				}
				uint8* destination[4] = { (uint8*)bitmap->Bits(), NULL, NULL, NULL };
				int strides[4] = { bitmap->BytesPerRow(), 0, 0, 0 };
				sws_scale(scaler, frame->data, frame->linesize, 0, frame->height,
					destination, strides);
				BBitmap* old = NULL;
				{
					BAutolock lock(fLock);
					if (generation != fGeneration) {
						delete bitmap;
						break;
					}
					old = fBitmap;
					fBitmap = bitmap;
					fFrames++;
				}
				delete old;
			}
			av_packet_unref(packet);
		}
	}

	sws_freeContext(scaler);
	avcodec_free_context(&decoder);
	av_frame_free(&frame);
	av_packet_free(&packet);
}
