#include "AirBands.h"

namespace {

const air_channel kSeoul[] = {
	{ "Emergency (guard)",		121500000ULL },
	{ "Incheon Tower",			118800000ULL },
	{ "Incheon Tower 2",		118200000ULL },
	{ "Incheon Ground",			121750000ULL },
	{ "Incheon Ground 2",		121875000ULL },
	{ "Incheon Delivery",		121600000ULL },
	{ "Incheon ATIS",			128650000ULL },
	{ "Gimpo Tower",			118100000ULL },
	{ "Gimpo Ground",			121900000ULL },
	{ "Gimpo ATIS",				126600000ULL },
	{ "Seoul Approach",			119100000ULL },
	{ "Seoul Approach 2",		119700000ULL },
	{ "Seoul Approach 3",		120800000ULL },
	{ "Seoul Departure",		121400000ULL },
	{ "Incheon Control",		124900000ULL },
	{ "Incheon Control 2",		126900000ULL },
};

const air_channel kTokyo[] = {
	{ "Emergency (guard)",		121500000ULL },
	{ "Haneda Tower",			118100000ULL },
	{ "Haneda Tower 2",			118225000ULL },
	{ "Haneda Ground",			121700000ULL },
	{ "Haneda Ground 2",		118625000ULL },
	{ "Haneda Delivery",		121825000ULL },
	{ "Haneda ATIS",			128800000ULL },
	{ "Narita Tower",			118200000ULL },
	{ "Narita Tower 2",			118350000ULL },
	{ "Narita Ground",			121850000ULL },
	{ "Narita Ground 2",		121600000ULL },
	{ "Narita ATIS",			128250000ULL },
	{ "Tokyo Approach",			119400000ULL },
	{ "Tokyo Approach 2",		124400000ULL },
	{ "Tokyo Departure",		126000000ULL },
	{ "Tokyo Control",			132300000ULL },
};

const air_city kCities[] = {
	{ "Seoul",	kSeoul,	(int)(sizeof(kSeoul) / sizeof(kSeoul[0])) },
	{ "Tokyo",	kTokyo,	(int)(sizeof(kTokyo) / sizeof(kTokyo[0])) },
};

} // namespace

int
AirCityCount()
{
	return (int)(sizeof(kCities) / sizeof(kCities[0]));
}

const air_city&
AirCityAt(int index)
{
	if (index < 0 || index >= AirCityCount())
		index = 0;
	return kCities[index];
}
