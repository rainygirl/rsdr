/*
 * Airband channel lists, grouped by city.
 *
 * Only 121.500 is certain - it is the international aeronautical emergency
 * frequency and does not change. Everything else here is from published
 * airport charts and is exactly the kind of thing that gets reassigned, so
 * treat the list as a starting point and confirm against the current AIP.
 *
 * The monitor does not depend on the list being right: it measures every
 * channel in the list from the spectrum and only demodulates one that is
 * actually carrying a signal, so a wrong entry costs nothing but a dead slot.
 */
#ifndef RSDR_AIR_BANDS_H
#define RSDR_AIR_BANDS_H

#include <SupportDefs.h>

struct air_channel {
	const char*	label;
	uint64		hz;
};

struct air_city {
	const char*			name;
	const air_channel*	channels;
	int					count;
};

int AirCityCount();
const air_city& AirCityAt(int index);

#endif // RSDR_AIR_BANDS_H
