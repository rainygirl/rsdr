/*
 * Runs the real demodulator over a captured I/Q file and writes a WAV.
 *
 * This exists because "I cannot hear anything" has at least four independent
 * causes - wrong frequency, wrong gain, a broken filter chain, and a broken
 * audio path - and this tool removes two of them from the picture. It links
 * Demodulator/Dsp exactly as the app does, with no BeAPI and no sound card
 * involved, so a WAV that sounds like a radio station proves the DSP is
 * correct and a WAV of hiss proves it is not.
 *
 *   rtl_sdr -S -f 92500000 -s 1058400 -g 350 -n 10584000 fm.iq
 *   demod_file fm.iq wfm 1058400 fm.wav
 *
 * The sample rate argument must be what rtl_sdr actually used, because the
 * decimation ratios are derived from it.
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Demodulator.h"

namespace {

struct wav_header {
	char	riff[4];
	uint32	riffSize;
	char	wave[4];
	char	fmt[4];
	uint32	fmtSize;
	uint16	format;
	uint16	channels;
	uint32	sampleRate;
	uint32	byteRate;
	uint16	blockAlign;
	uint16	bitsPerSample;
	char	data[4];
	uint32	dataSize;
};

bool
WriteWav(const char* path, const std::vector<short>& samples, uint32 rate)
{
	FILE* out = fopen(path, "wb");
	if (out == NULL) {
		fprintf(stderr, "cannot write %s\n", path);
		return false;
	}

	wav_header h;
	memcpy(h.riff, "RIFF", 4);
	memcpy(h.wave, "WAVE", 4);
	memcpy(h.fmt, "fmt ", 4);
	memcpy(h.data, "data", 4);
	h.fmtSize = 16;
	h.format = 1;
	h.channels = 1;
	h.sampleRate = rate;
	h.bitsPerSample = 16;
	h.blockAlign = 2;
	h.byteRate = rate * 2;
	h.dataSize = (uint32)(samples.size() * 2);
	h.riffSize = 36 + h.dataSize;

	fwrite(&h, sizeof(h), 1, out);
	if (!samples.empty())
		fwrite(&samples[0], 2, samples.size(), out);
	fclose(out);
	return true;
}

demod_mode
ParseMode(const char* name)
{
	if (strcasecmp(name, "wfm") == 0)
		return kModeWFM;
	if (strcasecmp(name, "am") == 0)
		return kModeAM;
	if (strcasecmp(name, "lsb") == 0)
		return kModeLSB;
	if (strcasecmp(name, "usb") == 0)
		return kModeUSB;
	return kModeCount;
}

} // namespace

int
main(int argc, char** argv)
{
	if (argc < 5) {
		fprintf(stderr,
			"usage: %s <iq-file> <wfm|nfm|am|lsb|usb> <tuner-rate> "
			"<out.wav> [fine-tune-hz]\n", argv[0]);
		return 1;
	}

	const char* inPath = argv[1];
	demod_mode mode = ParseMode(argv[2]);
	if (mode == kModeCount) {
		fprintf(stderr, "unknown mode %s\n", argv[2]);
		return 1;
	}
	uint32 tunerRate = (uint32)strtoul(argv[3], NULL, 10);
	const char* outPath = argv[4];
	float fineTune = argc > 5 ? (float)atof(argv[5]) : 0.0f;

	mode_plan plan = PlanForMode(mode);
	if (tunerRate != plan.tunerSampleRate) {
		fprintf(stderr, "warning: %s expects a %u Hz capture, got %u Hz - "
			"the decimation ratios will not give the nominal audio rate\n",
			ModeName(mode), (unsigned)plan.tunerSampleRate,
			(unsigned)tunerRate);
	}

	FILE* in = fopen(inPath, "rb");
	if (in == NULL) {
		fprintf(stderr, "cannot open %s\n", inPath);
		return 1;
	}

	Demodulator demod;
	demod.Configure(mode, tunerRate);
	demod.SetFineTune(fineTune);

	const size_t kBlock = 32768;
	std::vector<uint8> block(kBlock);
	std::vector<float> audio;
	std::vector<short> pcm;

	double peak = 0.0;
	double sumSquares = 0.0;
	size_t total = 0;
	float minLevel = 1e9f;
	float maxLevel = -1e9f;

	while (true) {
		size_t got = fread(&block[0], 1, kBlock, in);
		if (got < 2)
			break;

		audio.resize(demod.MaxAudioSamples(got));
		size_t produced = demod.Process(&block[0], got, &audio[0],
			audio.size());

		float level = demod.SignalLevelDb();
		if (level < minLevel)
			minLevel = level;
		if (level > maxLevel)
			maxLevel = level;

		for (size_t i = 0; i < produced; i++) {
			float v = audio[i];
			if (fabs(v) > peak)
				peak = fabs(v);
			sumSquares += (double)v * v;
			int s = (int)lrintf(v * 32767.0f);
			if (s > 32767)
				s = 32767;
			if (s < -32768)
				s = -32768;
			pcm.push_back((short)s);
		}
		total += produced;
	}
	fclose(in);

	if (total == 0) {
		fprintf(stderr, "no audio produced - input too short?\n");
		return 1;
	}

	double rms = sqrt(sumSquares / (double)total);
	printf("mode           %s\n", ModeName(mode));
	printf("tuner rate     %u Hz\n", (unsigned)tunerRate);
	printf("audio rate     %u Hz\n", (unsigned)plan.audioSampleRate);
	printf("audio samples  %lu (%.2f s)\n", (unsigned long)total,
		(double)total / (double)plan.audioSampleRate);
	printf("RF level       %.1f .. %.1f dBFS\n", minLevel, maxLevel);
	printf("audio peak     %.4f\n", peak);
	printf("audio RMS      %.4f (%.1f dBFS)\n", rms,
		20.0 * log10(rms + 1e-12));
	if (mode == kModeWFM) {
		printf("carrier offset %+.0f Hz\n", demod.CarrierOffsetHz());
	}

	if (!WriteWav(outPath, pcm, plan.audioSampleRate))
		return 1;
	printf("wrote          %s\n", outPath);
	return 0;
}
