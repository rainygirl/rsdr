/*
 * T-DMB Main Service Channel decoder: MSC soft bits in, MPEG-2 transport
 * stream packets out.
 *
 * Chain, in the order the receiver has to undo it (ETSI EN 300 401 for the DAB
 * layers, ETSI TS 102 427 for the T-DMB outer layer):
 *
 *   16-CIF time deinterleaving   bit i of a logical frame was delayed by
 *                                D[i mod 16] CIFs, D = 0,8,4,12,2,10,6,14,
 *                                1,9,5,13,3,11,7,15
 *   EEP depuncturing             back to the rate-1/4 mother code
 *   Viterbi, K = 7               0133, 0171, 0145, 0133
 *   energy dispersal             9-bit PRBS, reset every logical frame
 *   byte deinterleaving          Forney, I = 12, M = 17 - the T-DMB outer layer
 *   RS(204,188)                  shortened RS(255,239), t = 8
 *
 * Every stage was settled against a live capture of the MBC ensemble on 12A
 * (205.280 MHz) with two independent oracles: the FIC CRC for the frame
 * timing, and - decisively - RS syndromes, because twenty consecutive packets
 * decoding with *zero* corrections cannot happen unless every stage above is
 * right at once.
 *
 * Things that look like progress but are not, all of which cost real time:
 *
 *  - 0x47 landing every 204 bytes proves nothing about the byte deinterleaver.
 *    The Forney interleaver deliberately sends sync bytes through branch 0,
 *    which has zero delay, so they stay exactly 204 apart in the *interleaved*
 *    stream too. Measured 100% sync with the payload untouched.
 *  - Soft-versus-hard Viterbi agreement is not an error rate. At rate 2/3 hard
 *    decisions lose 2-3 dB, so 78% agreement was normal, not a bug.
 *  - The MSC needs sample-accurate timing for all 76 symbols, not just the
 *    three the FIC uses. A mid-frame sample drop leaves the FIC at 12/12 while
 *    costing the MSC 8 dB of MER; see DabProbe for the tracking that fixes it.
 */
#ifndef RSDR_DAB_MSC_H
#define RSDR_DAB_MSC_H

#include <SupportDefs.h>

#include <vector>

class DabMsc {
public:
	static const int kCifBits = 55296;			// 864 CU x 64 bits
	static const int kTimeDepth = 16;
	static const int kRsBlock = 204;
	static const int kTsPacket = 188;
	// Forney latency: every byte comes out (I-1)*M*I = 2244 bytes later, which
	// is exactly eleven RS blocks, so the first eleven are always garbage.
	static const int kDeintLatency = 2244;

	struct config {
		int		startCu;
		int		sizeCu;
		int		protectionLevel;	// 1-4, as decoded from FIG 0/1
		bool	eepOptionB;

		config()
			: startCu(0), sizeCu(0), protectionLevel(0), eepOptionB(false) {}
	};

	struct stats {
		uint64	cifsIn;
		uint64	framesOut;			// logical frames off the Viterbi
		uint64	blocksTried;		// 204-byte RS blocks examined
		uint64	rsOk;
		uint64	rsCorrected;		// bytes the RS decoder repaired
		bool	synced;				// RS block boundary found
		int		bitrateKbps;		// useful rate the profile implies

		stats()
			: cifsIn(0), framesOut(0), blocksTried(0), rsOk(0), rsCorrected(0),
			  synced(false), bitrateKbps(0) {}
	};

						DabMsc();

	// Works out the EEP profile from the subchannel size, which pins it: the
	// number of soft bits a profile consumes is fixed arithmetic, so requiring
	// it to equal sizeCu * 64 exactly leaves at most one candidate per
	// (option, level). Returns false when nothing fits, which means the FIC
	// was misread rather than that the signal is bad.
			bool		Configure(const config& c);
			void		Reset();

	// One CIF of MSC soft bits: kCifBits values, symbols 4..75 of a frame,
	// differentially demodulated in the same order the FIC uses. Returns how
	// many 188-byte TS packets this call produced.
			int			PushCif(const float* cifSoft);
			void		SetHardDecision(bool enabled) { fHardDecision = enabled; }
			void		SetQuantizedDecision(bool enabled) { fQuantizedDecision = enabled; }
	// Advance across CIFs that were not captured. This preserves the 16-CIF
	// time-interleaver phase without spending Viterbi time on known erasures.
			void		SkipCifs(uint64 count);

	const std::vector<uint8>& Packets() const { return fPackets; }
			void		ClearPackets() { fPackets.clear(); }
	const stats&		Stats() const { return fStats; }
			bool		Configured() const { return fConfigured; }

private:
			void		_BuildTrellis();
			bool		_PlanFor(int kbps, int level, bool optionB,
							int* l1, int* l2, int* consumed) const;
			void		_Depuncture(const float* in);
			void		_Viterbi(const float* soft, int nbits);
			void		_ViterbiHard(const float* soft, int nbits);
			void		_ViterbiQuantized(const float* soft, int nbits);
			void		_PushBytes(const uint8* bytes, int count);
			void		_Deinterleave(uint8 b);
			void		_TryBlocks();

			config				fConfig;
			bool				fConfigured;
			bool				fHardDecision;
			bool				fQuantizedDecision;
			stats				fStats;

			int					fSubBits;		// sizeCu * 64
			int					fUsefulBits;
			int					fMotherBits;
			int					fL1;
			int					fL2;
			const int8*			fPattern1;
			const int8*			fPattern2;

			uint8				fOutMask[64][2];
			int					fPred[64][2];
			int					fInBit[64];

	std::vector<float>			fRing;			// kTimeDepth x fSubBits
			uint64				fCifIndex;
	std::vector<float>			fInter;
	std::vector<float>			fCoded;
	std::vector<uint8>			fBits;
	std::vector<uint8>			fPrbs;
	std::vector<uint8>			fBytes;
	std::vector<float>			fMetric;
	std::vector<float>			fNextMetric;
	std::vector<uint64>			fMetricPick;

	// Outer layer. The raw stream is held only until the RS block boundary is
	// found; after that bytes go straight through the deinterleaver.
	std::vector<uint8>			fRaw;
			bool				fSynced;
			// Consecutive RS-block failures since the last success. A long run
			// means the byte boundary drifted (e.g. a CIF discontinuity at a live
			// USB read boundary); once that happens fSynced would otherwise stay
			// wrong forever, so re-search the boundary. See _Deinterleave.
			int					fRsFailStreak;
			int					fBranchPhase;
	std::vector<std::vector<uint8> >	fBranch;
	std::vector<int>			fBranchPos;
			int					fDeintIndex;
	std::vector<uint8>			fBlock;
			int					fBlockFill;
			int					fSkip;

	std::vector<uint8>			fPackets;
};

#endif // RSDR_DAB_MSC_H
