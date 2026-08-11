#include "Bands.h"

namespace {

const band_plan kBandPlans[] = {
	// Below 28.8 MHz SdrDevice bypasses the R820T2 and selects the RTL2832U Q
	// ADC input. Dongles with that input routed to the antenna can therefore
	// cover the broadcast bands below VHF without an upconverter.
	{ "LW broadcast",       148500ULL,      283500ULL,    1000, kModeAM },
	{ "MW broadcast",       520000ULL,     1710000ULL,    9000, kModeAM },
	{ "SW broadcast",      1800000ULL,    28800000ULL,    5000, kModeAM },
	{ "FM broadcast",	 87500000ULL,	108000000ULL,	100000,	kModeWFM },
	{ "T-DMB (Band III)",174000000ULL,216000000ULL,	1000,	kModeDMB },
	{ "2 m amateur",	144000000ULL,	148000000ULL,	  1000,	kModeUSB },
};

const preset kPresets[] = {
	// Direct-sampling entry points. Below 28.8 MHz RtlDevice selects the
	// RTL2832U Q ADC automatically, bypassing the R820T tuner. These are band
	// landmarks rather than claims about a particular broadcaster being on
	// air; they make the MW/SW decoder path reachable from the macOS UI.
	{ "MW 603 kHz (AM)",          603000ULL, kModeAM },
	{ "MW 711 kHz (AM)",          711000ULL, kModeAM },
	{ "MW 972 kHz (AM)",          972000ULL, kModeAM },
	{ "MW 1134 kHz (AM)",        1134000ULL, kModeAM },
	{ "SW 3.900 MHz (AM)",       3900000ULL, kModeAM },
	{ "SW 5.900 MHz (AM)",       5900000ULL, kModeAM },
	{ "SW 7.200 MHz (AM)",       7200000ULL, kModeAM },
	{ "SW 9.600 MHz (AM)",       9600000ULL, kModeAM },
	{ "SW 11.800 MHz (AM)",     11800000ULL, kModeAM },
	{ "SW 15.200 MHz (AM)",     15200000ULL, kModeAM },
	{ "40 m 7.100 MHz (LSB)",    7100000ULL, kModeLSB },
	{ "20 m 14.200 MHz (USB)",  14200000ULL, kModeUSB },
	{ "15 m 21.200 MHz (USB)",  21200000ULL, kModeUSB },

	// Republic of Korea eAIP, RKSS AD 2.18: Seoul Approach includes 119.100
	// MHz. Air defaults to this single-frequency entry; city-wide scanning is
	// intentionally disabled because weak carriers and local noise could not be
	// distinguished reliably enough to drive automatic retuning.
	{ "RKSS Approach 119.100 MHz (Air)", 119100000ULL, kModeAir },

	// FM entries below were measured on this machine with this dongle: a
	// 2.048 MS/s capture at each of 89/91/.../107 MHz, averaged periodogram,
	// peaks more than 12 dB over the local noise floor. The two strongest by
	// a wide margin (+44 dB) are 92.5 and 96.7, which makes them the useful
	// ones for checking that audio works at all.
	{ "FM 89.1 MHz",		 89100000ULL,	kModeWFM },
	{ "FM 92.5 MHz",		 92500000ULL,	kModeWFM },
	{ "FM 93.1 MHz",		 93100000ULL,	kModeWFM },
	{ "FM 96.7 MHz",		 96700000ULL,	kModeWFM },
	{ "FM 99.1 MHz",		 99100000ULL,	kModeWFM },
	{ "FM 102.7 MHz",		102700000ULL,	kModeWFM },
	{ "FM 104.5 MHz",		104500000ULL,	kModeWFM },
	{ "FM 105.3 MHz",		105300000ULL,	kModeWFM },
	{ "FM 106.1 MHz",		106100000ULL,	kModeWFM },
	{ "FM 107.7 MHz",		107700000ULL,	kModeWFM },

	// Korean T-DMB blocks that are actually on the air here, found by scanning
	// the whole of Band III: 8B, 12A, 12B and 12C carry ensembles and nothing
	// else in 174-216 MHz does. Channels 7, 9, 10, 11 and 13 were probed and
	// are empty at this location, so they are not listed - they were only ever
	// useful as negative controls while the DAB probe was being written.
	// Each was captured at 2.048 MS/s and put through the full DAB Mode I
	// check - null symbol every 96 ms, guard-interval lock, pi/4-DQPSK
	// constellation - and 12A decodes all the way to a transport stream.
	// 9C is worth remembering as the counter-example: it carries something
	// narrow and looks like a signal on a power meter, but has no DAB
	// structure at all.
	{ "T-DMB 8B  183.008 (on air)", 183008000ULL,	kModeDMB },
	{ "T-DMB 12A 205.280 (on air)", 205280000ULL,	kModeDMB },
	{ "T-DMB 12B 207.008 (on air)", 207008000ULL,	kModeDMB },
	{ "T-DMB 12C 208.736 (on air)", 208736000ULL,	kModeDMB },

	// NTSC-M sound carriers: standard vision carrier + 4.5 MHz.  These use
	// the existing WFM demodulator; no NTSC video decoder is involved.
#define NTSC_AUDIO(channel, videoHz) \
	{ "NTSC TV " #channel " audio", (videoHz##ULL + 4500000ULL), kModeWFM },
	NTSC_AUDIO(2, 55250000)
	NTSC_AUDIO(3, 61250000)
	NTSC_AUDIO(4, 67250000)
	NTSC_AUDIO(5, 77250000)
	NTSC_AUDIO(6, 83250000)
	NTSC_AUDIO(7, 175250000)
	NTSC_AUDIO(8, 181250000)
	NTSC_AUDIO(9, 187250000)
	NTSC_AUDIO(10, 193250000)
	NTSC_AUDIO(11, 199250000)
	NTSC_AUDIO(12, 205250000)
	NTSC_AUDIO(13, 211250000)
	NTSC_AUDIO(14, 471250000)
	NTSC_AUDIO(15, 477250000)
	NTSC_AUDIO(16, 483250000)
	NTSC_AUDIO(17, 489250000)
	NTSC_AUDIO(18, 495250000)
	NTSC_AUDIO(19, 501250000)
	NTSC_AUDIO(20, 507250000)
	NTSC_AUDIO(21, 513250000)
	NTSC_AUDIO(22, 519250000)
	NTSC_AUDIO(23, 525250000)
	NTSC_AUDIO(24, 531250000)
	NTSC_AUDIO(25, 537250000)
	NTSC_AUDIO(26, 543250000)
	NTSC_AUDIO(27, 549250000)
	NTSC_AUDIO(28, 555250000)
	NTSC_AUDIO(29, 561250000)
	NTSC_AUDIO(30, 567250000)
	NTSC_AUDIO(31, 573250000)
	NTSC_AUDIO(32, 579250000)
	NTSC_AUDIO(33, 585250000)
	NTSC_AUDIO(34, 591250000)
	NTSC_AUDIO(35, 597250000)
	NTSC_AUDIO(36, 603250000)
	NTSC_AUDIO(37, 609250000)
	NTSC_AUDIO(38, 615250000)
	NTSC_AUDIO(39, 621250000)
	NTSC_AUDIO(40, 627250000)
	NTSC_AUDIO(41, 633250000)
	NTSC_AUDIO(42, 639250000)
	NTSC_AUDIO(43, 645250000)
	NTSC_AUDIO(44, 651250000)
	NTSC_AUDIO(45, 657250000)
	NTSC_AUDIO(46, 663250000)
	NTSC_AUDIO(47, 669250000)
	NTSC_AUDIO(48, 675250000)
	NTSC_AUDIO(49, 681250000)
	NTSC_AUDIO(50, 687250000)
	NTSC_AUDIO(51, 693250000)
	NTSC_AUDIO(52, 699250000)
	NTSC_AUDIO(53, 705250000)
	NTSC_AUDIO(54, 711250000)
	NTSC_AUDIO(55, 717250000)
	NTSC_AUDIO(56, 723250000)
	NTSC_AUDIO(57, 729250000)
	NTSC_AUDIO(58, 735250000)
	NTSC_AUDIO(59, 741250000)
	NTSC_AUDIO(60, 747250000)
	NTSC_AUDIO(61, 753250000)
	NTSC_AUDIO(62, 759250000)
	NTSC_AUDIO(63, 765250000)
	NTSC_AUDIO(64, 771250000)
	NTSC_AUDIO(65, 777250000)
	NTSC_AUDIO(66, 783250000)
	NTSC_AUDIO(67, 789250000)
	NTSC_AUDIO(68, 795250000)
	NTSC_AUDIO(69, 801250000)
#undef NTSC_AUDIO


};

} // namespace

int
BandPlanCount()
{
	return (int)(sizeof(kBandPlans) / sizeof(kBandPlans[0]));
}

const band_plan&
BandPlanAt(int index)
{
	if (index < 0)
		index = 0;
	if (index >= BandPlanCount())
		index = BandPlanCount() - 1;
	return kBandPlans[index];
}

int
BandPlanForFrequency(uint64 hz)
{
	for (int i = 0; i < BandPlanCount(); i++) {
		if (hz >= kBandPlans[i].lowHz && hz <= kBandPlans[i].highHz)
			return i;
	}
	return -1;
}

int
PresetCount()
{
	return (int)(sizeof(kPresets) / sizeof(kPresets[0]));
}

const preset&
PresetAt(int index)
{
	if (index < 0)
		index = 0;
	if (index >= PresetCount())
		index = PresetCount() - 1;
	return kPresets[index];
}
