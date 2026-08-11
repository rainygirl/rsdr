/*
 * DAB / T-DMB reception probe. Not a decoder.
 *
 * It answers one question - "is a real DAB Mode I signal present and
 * synchronisable?" - and answers it the way that cannot be faked by a
 * wideband blob of power at the right frequency. Every step below either
 * finds the structure the standard mandates or it does not:
 *
 *   null symbol    2656 samples of near-silence recurring every 96 ms
 *   guard lock     each symbol's first 504 samples repeat its own tail from
 *                  2048 samples later, which is what makes it OFDM and gives
 *                  sample-exact symbol timing
 *   constellation  differential demodulation between consecutive symbols
 *                  lands on four points, because DAB is pi/4-DQPSK
 *
 * Verified against live captures of the four multiplexes receivable here
 * (8B 183.008, 12A 205.280, 12B 207.008, 12C 208.736 MHz): all four give MER
 * between 9.2 and 10.5 dB, while 9C 190.736 - which carries something narrow
 * rather than a DAB block - fails the null test, as it should.
 *
 * Two things learned while building this, both of which look like bugs if you
 * do not know them:
 *
 *  - Do NOT correct the coarse frequency offset. DAB carries data
 *    differentially in time on each carrier, so an offset of a whole number
 *    of 1 kHz carrier spacings cancels out completely; only the fractional
 *    part matters, and the guard-interval phase measures exactly that.
 *    Estimating the coarse offset from the spectrum made MER *worse*
 *    (8.6 -> 7.0 dB), because the neighbouring multiplex 1.728 MHz away puts
 *    its band edge inside a 2.048 MHz window and drags the estimate to a
 *    value that is not a multiple of 1 kHz.
 *
 *  - The null symbol is not equally deep every frame. It carries the
 *    transmitter identification signal, so measured depth here alternated
 *    between 0.043 and 0.076 of mean power frame to frame, which moves the
 *    minimum around by up to a thousand samples. Deriving the frame period
 *    from successive null positions therefore gives nonsense (it read as a
 *    5000 ppm clock error); the symbol period from the guard correlation is
 *    the reliable measurement, and it comes out at exactly the nominal 2552.
 */
#ifndef RSDR_DAB_PROBE_H
#define RSDR_DAB_PROBE_H

#include <SupportDefs.h>

#include <vector>

#include "DabFic.h"
#include "Dsp.h"

class DabProbe {
public:
	// DAB Mode I at 2.048 MS/s (ETSI EN 300 401).
	static const uint32 kSampleRate = 2048000;
	static const int kFrameSamples = 196608;	// 96 ms
	static const int kNullSamples = 2656;
	static const int kSymbolSamples = 2552;
	static const int kUsefulSamples = 2048;
	static const int kGuardSamples = 504;
	static const int kSymbolsPerFrame = 76;
	static const int kCarriersPerSide = 768;

	// Symbols the constellation measurement uses. All 76 are not needed to
	// tell a locked signal from noise, and asking for them makes the window
	// below much larger for no benefit.
	static const int kMeasureSymbols = 24;

	// Samples one analysis needs.
	//
	// This has to hold a whole frame period *plus* everything the analysis
	// consumes after the null it finds, because the null can sit anywhere in
	// that period. Getting it wrong is not obvious from the outside: with a
	// window of one frame plus a little slack, the null search range collapsed
	// to the first 10208 samples - 5 ms out of a 96 ms period - and the probe
	// locked on 6% of attempts while a contiguous offline capture of the same
	// signal locked on every single frame.
	static const int kWindowSamples = kFrameSamples + kNullSamples
		+ (kMeasureSymbols + 2) * kSymbolSamples;

	struct result {
		bool	locked;
		float	nullDepth;		// fraction of mean power; a real null is <0.3
		float	merDb;			// pi/4-DQPSK error vector magnitude, as dB
		float	fineOffsetHz;	// fractional carrier offset, from the guard
		int		timingShift;	// samples, relative to the null's end
		int		symbolsUsed;
		uint32	analyses;
		uint32	locks;
	// Filled once the FIC decodes: what the multiplex actually says it is.
		uint32	fibsOk;
		uint32	fibsTried;
		int		fibsThisFrame;

		result()
			:
			locked(false),
			nullDepth(1.0f),
			merDb(0.0f),
			fineOffsetHz(0.0f),
			timingShift(0),
			symbolsUsed(0),
			analyses(0),
			locks(0),
			fibsOk(0),
			fibsTried(0),
			fibsThisFrame(0)
		{
		}
	};

						DabProbe();

			void		Reset();
	// Raw RTL2832 bytes at kSampleRate. Blocks are ignored until the analysis
	// interval has elapsed, so the cost is one analysis a second and nothing
	// in between - which matters on a single-core machine that is also
	// running the display.
	//
	// The block MUST be contiguous samples: OFDM synchronisation cannot
	// bridge a gap. SdrDevice reads two whole DAB frames per USB transfer at
	// this rate for exactly that reason, so one block always contains a
	// complete frame.
			void		Feed(const uint8* iq, size_t bytes);

	const result&		Result() const { return fResult; }
	// Ensemble and service list, accumulated across analyses - the FIGs that
	// carry them are spread over several frames, so they arrive gradually.
			const DabFic&	Fic() const { return fFic; }

private:
			void		_Analyze(const dsp::Cf* x, size_t count);
			void		_BuildMapper();
			float		_FineFrequency(const dsp::Cf* x, int start,
							int symbols) const;
			int			_Timing(const dsp::Cf* x, size_t count, int start,
							int search) const;

			result				fResult;
			DabFic				fFic;
	// Frequency interleaving permutation: logical bit index -> FFT bin.
	std::vector<int>			fMapper;
	std::vector<float>			fFicSoft;
			bigtime_t			fLastAnalysis;
	std::vector<dsp::Cf>		fWindow;
	std::vector<double>			fPowerSum;
	std::vector<dsp::Cf>		fSymbol;
};

#endif // RSDR_DAB_PROBE_H
