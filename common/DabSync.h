/*
 * DAB Mode I frame synchronisation with per-symbol timing tracking.
 *
 * The FIC survives on frame-level timing alone; the MSC does not. On this
 * machine the dongle drops samples *inside* a single USB transfer, and a drop
 * of a few hundred samples mid-frame pushes every later FFT window past the
 * useful part into the next symbol's guard. Measured cost: FIC still 12/12,
 * MSC MER 15.4 -> 7.1 dB, raw channel BER 0.4% -> 7.4%, which is the
 * difference between a rate-2/3 subchannel working and not existing.
 *
 * Two traps are baked into the design here, both of which produced
 * confident-looking wrong answers first:
 *
 *  - Guard correlation is unambiguous but far too broad in an SFN to track a
 *    symbol at a time; searching it freely pinned itself to the edge of a
 *    +-450 sample window and destroyed even the FIC.
 *
 *  - Constellation quality is precise but ambiguous. Shifting the window by
 *    delta rotates carrier k by 2*pi*k*delta/2048, and at delta = 512 that is
 *    a whole multiple of 90 degrees on every carrier - so the points still land
 *    exactly on the QPSK constellation and both a u^4 metric and a
 *    nearest-ideal-point MER call it perfect, while the decoded bits are wrong,
 *    because DQPSK reads its bits off the quadrant.
 *
 * What resolves it is a measurement rather than a better metric: every
 * frame-to-frame spacing deficit seen on this hardware is a whole multiple of
 * 94 samples (188 bytes), never anything else. Restricting candidate positions
 * to multiples of 94 removes the ambiguity for free, since the smallest k, m
 * with 94k = 512m is k = 256 - 24064 samples, far outside any real drop. Drift
 * is also forced to be non-increasing: samples get lost, never invented.
 *
 * With that, all fifteen whole frames of a 1.5 s capture kept the FIC at 12/12
 * and eight consecutive frames held MSC MER at ~15 dB, which was enough to
 * feed the 16-CIF time deinterleaver and get RS(204,188) blocks that decode
 * with zero corrections.
 */
#ifndef RSDR_DAB_SYNC_H
#define RSDR_DAB_SYNC_H

#include <SupportDefs.h>

#include <vector>

#include "Dsp.h"

class DabSync {
public:
	static const uint32 kSampleRate = 2048000;
	static const int kFrameSamples = 196608;
	static const int kNullSamples = 2656;
	static const int kSymbolSamples = 2552;
	static const int kUsefulSamples = 2048;
	static const int kGuardSamples = 504;
	static const int kSymbolsPerFrame = 76;
	static const int kCarriers = 1536;
	static const int kSoftPerSymbol = 2 * kCarriers;
	// The measured drop quantum on this hardware, in samples (188 bytes).
	static const int kDropQuantum = 94;
	static const int kMaxDropSteps = 8;

	// How many symbols in a frame may pay for a drop search.
	//
	// A drop shifts the grid once and it stays shifted, so the number of
	// symbols that genuinely need searching equals the number of drops - one
	// to four per frame on this hardware. Anything beyond that is the quality
	// metric being fooled by noise, and on a weak signal that is most of the
	// frame: 75 searches x 12 FFTs x 10.4 frames a second is about 14,900
	// 2048-point FFTs a second, which on a 1.33 GHz Atom is over 100% of one
	// core by itself. That is why load stayed above real time no matter which
	// service was selected - the cost was in the front end, not the Viterbi.
	//
	// Past the budget the nominal position is used. On a bad signal that
	// degrades the output rather than degrading it *and* burning the CPU.
	// Haiku Atom quality/speed tradeoff: one expensive timing search per
	// frame is enough to follow the dominant USB drop. Remaining symbols use
	// the nominal grid, keeping the front-end closer to real time.
	static const int kSearchesPerFrame = 1;

						DabSync();

	// The buffer is borrowed, not copied, and must stay alive and unchanged
	// for as long as it is used.
			void		SetBuffer(const dsp::Cf* x, size_t count);
			// Once FIC has described the selected MSC service, symbols 1-2
			// are no longer needed. Symbol 3 remains the differential anchor.
			void		SetSkipFic(bool enabled) { fSkipFic = enabled; }

	// Locate the null and the first symbol at or after `from`. Returns the
	// position of symbol 0 (its guard, not its useful part), or -1.
			int			FindFrame(size_t from, float* fineHz,
							float* nullDepth);
	// Align the frame following one just tracked. `expected` is normally the
	// previous symbol 75 position plus symbol+null lengths.
			int			NextFrame(int expected, float* fineHz);

	// Fill positions[kSymbolsPerFrame] by walking symbol to symbol. Returns
	// false when the frame runs off the end of the buffer.
			bool		TrackFrame(int start, float fineHz, int* positions);

	// Differential soft bits for symbols 1..75: 75 * kSoftPerSymbol values,
	// all 1536 real parts of a symbol then all 1536 imaginary parts, which is
	// the order DabFic and DabMsc expect.
			void		SoftBits(const int* positions, float fineHz,
							float* out);

	const std::vector<int>&	Mapper() const { return fMapper; }

private:
			void		_BuildMapper();
			bool		_Spectrum(int pos, float fineHz, dsp::Cf* out);
			float		_Quality(const dsp::Cf* cur, const dsp::Cf* prev) const;
			int			_Timing(int start, int search) const;
			int			_NextTiming(int expected) const;
			float		_FineFrequency(int start, int symbols) const;

	const dsp::Cf*		fBuffer;
			size_t		fCount;
	std::vector<int>	fMapper;
	std::vector<int>	fMapperBitReversed;
	std::vector<dsp::Cf>	fScratch;
	std::vector<dsp::Cf>	fCur;
	std::vector<dsp::Cf>	fPrev;
	std::vector<dsp::Cf>	fBest;
	std::vector<dsp::Cf>	fNominal;
	// Every symbol's spectrum as tracking settled on it, so SoftBits does not
	// have to FFT the whole frame a second time.
	std::vector<dsp::Cf>	fSpec;
			bool			fSpecValid;
			bool			fSkipFic;
};

#endif // RSDR_DAB_SYNC_H
