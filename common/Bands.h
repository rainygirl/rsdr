/*
 * Band plans and presets.
 *
 * The Korean T-DMB channel table is the interesting one: Korea did not adopt
 * the European DAB raster (5A = 174.928 MHz and so on). It divides each 6 MHz
 * Band III television channel into three 1.536 MHz DMB blocks spaced
 * 1.728 MHz apart, so the frequencies are entirely different numbers. Getting
 * this wrong means tuning 350 kHz off and seeing nothing at all.
 */
#ifndef RSDR_BANDS_H
#define RSDR_BANDS_H

#include <SupportDefs.h>

#include "Demodulator.h"

struct band_plan {
	const char*	name;
	uint64		lowHz;
	uint64		highHz;
	uint32		stepHz;
	demod_mode	defaultMode;
};

struct preset {
	const char*	label;
	uint64		frequencyHz;
	demod_mode	mode;
};

int BandPlanCount();
const band_plan& BandPlanAt(int index);
// Band plan containing the frequency, or -1.
int BandPlanForFrequency(uint64 hz);

int PresetCount();
const preset& PresetAt(int index);

#endif // RSDR_BANDS_H
