#include "bsac_api.h"

#include <stdlib.h>
#include <string.h>

#include "libavcodec/avcodec.h"

struct bsac_decoder {
	AVCodecContext* context;
};

extern AVCodec ff_aac_decoder;

bsac_decoder*
bsac_decoder_create(const uint8_t* config, size_t config_size)
{
	if (config == NULL || config_size == 0)
		return NULL;
	AVCodec* codec = &ff_aac_decoder;
	if (codec == NULL)
		return NULL;
	bsac_decoder* decoder = (bsac_decoder*)av_mallocz(sizeof(*decoder));
	if (decoder == NULL)
		return NULL;
	decoder->context = avcodec_alloc_context3(codec);
	if (decoder->context == NULL) {
		av_free(decoder);
		return NULL;
	}
	decoder->context->extradata = av_malloc(config_size
		+ FF_INPUT_BUFFER_PADDING_SIZE);
	if (decoder->context->extradata == NULL) {
		av_free(decoder->context);
		av_free(decoder);
		return NULL;
	}
	memcpy(decoder->context->extradata, config, config_size);
	memset(decoder->context->extradata + config_size, 0,
		FF_INPUT_BUFFER_PADDING_SIZE);
	decoder->context->extradata_size = (int)config_size;
	if (avcodec_open(decoder->context, codec) < 0) {
		bsac_decoder_destroy(decoder);
		return NULL;
	}
	return decoder;
}

void
bsac_decoder_destroy(bsac_decoder* decoder)
{
	if (decoder == NULL)
		return;
	if (decoder->context != NULL) {
		avcodec_close(decoder->context);
		av_free(decoder->context->extradata);
		av_free(decoder->context);
	}
	av_free(decoder);
}

int
bsac_decoder_decode(bsac_decoder* decoder, const uint8_t* access_unit,
	size_t access_unit_size, int16_t* pcm, size_t pcm_capacity_samples)
{
	if (decoder == NULL || decoder->context == NULL || access_unit == NULL
		|| access_unit_size == 0 || pcm == NULL) {
		return -1;
	}
	AVPacket packet;
	av_init_packet(&packet);
	packet.data = (uint8_t*)access_unit;
	packet.size = (int)access_unit_size;
	int bytes = (int)(pcm_capacity_samples * sizeof(int16_t));
	int used = avcodec_decode_audio3(decoder->context, pcm, &bytes, &packet);
	if (used < 0)
		return -1;
	return bytes / (int)sizeof(int16_t);
}

int
bsac_decoder_sample_rate(const bsac_decoder* decoder)
{
	return decoder != NULL && decoder->context != NULL
		? decoder->context->sample_rate : 0;
}

int
bsac_decoder_channels(const bsac_decoder* decoder)
{
	return decoder != NULL && decoder->context != NULL
		? decoder->context->channels : 0;
}
