/*
 * DAB Fast Information Channel decoder: soft bits in, ensemble and service
 * list out.
 *
 * Every convention in here was settled against live captures with the FIB
 * CRC as the oracle, not read off a diagram - a 16-bit CRC over each 256-bit
 * block is unforgeable, so a chain that produces 12 passing FIBs out of 12 is
 * right in every stage at once. On the four multiplexes receivable here that
 * came out between 90% and 100% of FIBs.
 *
 * The conventions that had to be pinned down, each of which has an equally
 * plausible-looking opposite:
 *
 *   soft bit sign     positive means the encoder emitted a 0
 *   bit order         all 1536 real parts of a symbol, then all 1536
 *                     imaginary parts - not interleaved per carrier
 *   carrier order     logical bit j comes from carrier mapper[j], where the
 *                     mapper is the standard's frequency interleaving
 *                     permutation, generated rather than tabulated
 *   generators        0133, 0171, 0145, 0133 with the MSB as the newest input
 *                     bit; reversing the bit order decodes nothing
 *   coarse frequency  deliberately NOT corrected: DQPSK is differential in
 *                     time, so a whole number of 1 kHz carrier spacings
 *                     cancels out, and correcting it from the spectrum made
 *                     things worse because a neighbouring multiplex 1.728 MHz
 *                     away drags the estimate off a multiple of 1 kHz
 */
#ifndef RSDR_DAB_FIC_H
#define RSDR_DAB_FIC_H

#include <SupportDefs.h>

#include <string>
#include <vector>

class DabFic {
public:
	// One FIC block: 2304 soft bits, depunctured to 3096, Viterbi decoded to
	// 774 of which 768 are data, giving three 256-bit FIBs. Four blocks per
	// 96 ms frame, so twelve FIBs.
	static const int kSoftBitsPerBlock = 2304;
	static const int kCodedBits = 3096;
	static const int kDataBits = 768;
	static const int kBlocksPerFrame = 4;
	static const int kFibBits = 256;

	struct service {
		uint32		id;
		std::string	label;
		int			subchannel;		// -1 when not a stream component
		int			type;			// ASCTy for audio, DSCTy for data
		int			scid;			// 12-bit packet component id, or -1
		bool		isAudio;
		bool		isPrimary;
		bool		conditionalAccess;

		service()
			: id(0), subchannel(-1), type(-1), scid(-1), isAudio(false),
			  isPrimary(false), conditionalAccess(false)
		{
		}
	};

	struct subchannel {
		int	id;
		int	startCu;
		int	sizeCu;
		int	protectionLevel;	// 1-4
		bool eepOptionB;
		bool shortForm;

		subchannel()
			: id(-1), startCu(0), sizeCu(0), protectionLevel(0),
			  eepOptionB(false), shortForm(false)
		{
		}
	};

						DabFic();

			void		Reset();
	// softBits must be 3 * 3072 values: symbols 1, 2 and 3 of the frame,
	// differentially demodulated and ordered as described above.
	// Returns how many of the twelve FIBs passed CRC.
			int			DecodeFrame(const float* softBits);

			uint32		EnsembleId() const { return fEnsembleId; }
	const std::string&	EnsembleLabel() const { return fEnsembleLabel; }
	const std::vector<service>& Services() const { return fServices; }
	const std::vector<subchannel>& Subchannels() const { return fSubchannels; }
			uint64		FibsOk() const { return fFibsOk; }
			uint64		FibsTried() const { return fFibsTried; }

private:
			void		_Depuncture(const float* in, float* out) const;
			void		_Viterbi(const float* soft, int nbits, uint8* bits);
			void		_ParseFib(const uint8* fib);
			void		_ParseFig0(const uint8* body, int length);
			void		_ParseFig1(const uint8* body, int length);
			service*	_ServiceFor(uint32 id);

	// Trellis for the rate-1/4 K=7 mother code, built once.
			uint8		fOutMask[64][2];
			int			fPred[64][2];
			int			fInBit[64];

	std::vector<uint8>	fPrbs;
	std::vector<float>	fCoded;
	std::vector<uint8>	fDecoded;
	std::vector<uint64>	fMetricPick;
	std::vector<float>	fMetric;
	std::vector<float>	fNextMetric;

			uint32		fEnsembleId;
			std::string	fEnsembleLabel;
	std::vector<service>	fServices;
	std::vector<subchannel>	fSubchannels;
	// FIG 0/3 may arrive before FIG 0/2. Keep its packet-component mapping by
	// 12-bit SCId so either signalling order resolves to a SubChId/DSCTy.
			int				fPacketSubchannel[4096];
			int				fPacketType[4096];
			uint64		fFibsOk;
			uint64		fFibsTried;
};

#endif // RSDR_DAB_FIC_H
