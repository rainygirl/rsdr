#ifndef RSDR_BSAC_API_H
#define RSDR_BSAC_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bsac_decoder bsac_decoder;

bsac_decoder* bsac_decoder_create(const uint8_t* config, size_t config_size);
void bsac_decoder_destroy(bsac_decoder* decoder);
int bsac_decoder_decode(bsac_decoder* decoder, const uint8_t* access_unit,
	size_t access_unit_size, int16_t* pcm, size_t pcm_capacity_samples);
int bsac_decoder_sample_rate(const bsac_decoder* decoder);
int bsac_decoder_channels(const bsac_decoder* decoder);

#ifdef __cplusplus
}
#endif

#endif
