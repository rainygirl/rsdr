#include "DabMsc.h"

#include <algorithm>
#include <cstring>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

#include "DabTables.h"

namespace {

// Time interleaving delays, in CIFs, for bit index i mod 16 (EN 300 401 12).
const int kDelay[16] = { 0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15 };

// Consecutive RS-block failures that trigger a byte-boundary re-search.
// ~2 CIFs of subch-1 blocks; long enough to ignore isolated bursts, short
// enough to recover an audible fraction of a second after a discontinuity.
const int kRsResyncFailures = 24;

// ------------------------------------------------------ RS(204,188), t = 8
//
// The shortened RS(255,239) that DVB and T-DMB share: GF(256) with primitive
// polynomial x^8+x^4+x^3+x^2+1, alpha = 2, generator roots alpha^0..alpha^15.
//
// Validated against a self-encoder before being pointed at real data: corrects
// 0-8 byte errors every time, rejects 9 or more, never misdecodes. Two bugs had
// to be found that way and neither is visible by inspection - the generator was
// being built as (alpha^j x + 1) instead of (x + alpha^j), giving roots at
// alpha^-j, and Forney needs the X_i factor that a first root of alpha^0
// implies, without which every magnitude comes out scaled by 1/X_i.

const int kRsRoots = 16;
const int kRsBytes = 204;

uint8 gExp[512];
uint8 gLog[256];
bool gTablesReady = false;

void
BuildGf()
{
	if (gTablesReady)
		return;
	int x = 1;
	for (int i = 0; i < 255; i++) {
		gExp[i] = (uint8)x;
		gLog[x] = (uint8)i;
		x <<= 1;
		if (x & 0x100)
			x ^= 0x11D;
	}
	for (int i = 255; i < 512; i++)
		gExp[i] = gExp[i - 255];
	gTablesReady = true;
}

inline uint8
GfMul(uint8 a, uint8 b)
{
	if (a == 0 || b == 0)
		return 0;
	return gExp[(int)gLog[a] + (int)gLog[b]];
}

inline uint8
GfInv(uint8 a)
{
	return gExp[255 - (int)gLog[a]];
}

// Corrects in place. Returns the number of errors repaired, or -1 when the
// block carries more than the code can resolve.
int
RsDecode(uint8* r)
{
	BuildGf();

	uint8 synd[kRsRoots];
	bool any = false;
	for (int j = 0; j < kRsRoots; j++) {
		uint8 s = 0;
		for (int i = 0; i < kRsBytes; i++)
			s = (uint8)(GfMul(s, gExp[j]) ^ r[i]);
		synd[j] = s;
		if (s)
			any = true;
	}
	if (!any)
		return 0;

	// Berlekamp-Massey.
	uint8 lam[kRsRoots + 2];
	uint8 prev[kRsRoots + 2];
	uint8 tmp[kRsRoots + 2];
	memset(lam, 0, sizeof(lam));
	memset(prev, 0, sizeof(prev));
	lam[0] = 1;
	prev[0] = 1;
	int lamLen = 1;
	int prevLen = 1;
	int L = 0;
	int m = 1;
	uint8 bb = 1;

	for (int n = 0; n < kRsRoots; n++) {
		uint8 delta = synd[n];
		for (int i = 1; i <= L && i < lamLen; i++)
			delta ^= GfMul(lam[i], synd[n - i]);
		if (delta == 0) {
			m++;
			continue;
		}
		uint8 scale = GfMul(delta, GfInv(bb));
		memcpy(tmp, lam, sizeof(tmp));
		int tmpLen = lamLen;
		int need = m + prevLen;
		if (need > lamLen) {
			if (need > kRsRoots + 1)
				return -1;
			lamLen = need;
		}
		for (int i = 0; i < prevLen; i++)
			lam[i + m] ^= GfMul(scale, prev[i]);
		if (2 * L <= n) {
			L = n + 1 - L;
			memcpy(prev, tmp, sizeof(tmp));
			prevLen = tmpLen;
			bb = delta;
			m = 1;
		} else
			m++;
	}

	int deg = lamLen - 1;
	while (deg > 0 && lam[deg] == 0)
		deg--;
	if (deg == 0 || deg > kRsRoots / 2)
		return -1;

	// Chien search over the positions this shortened code occupies.
	int pos[kRsRoots / 2];
	uint8 xinv[kRsRoots / 2];
	int found = 0;
	for (int i = 0; i < kRsBytes; i++) {
		int power = (kRsBytes - 1 - i) % 255;
		uint8 xi = gExp[(255 - power) % 255];
		uint8 acc = 0;
		uint8 xp = 1;
		for (int c = 0; c <= deg; c++) {
			acc ^= GfMul(lam[c], xp);
			xp = GfMul(xp, xi);
		}
		if (acc == 0) {
			if (found >= kRsRoots / 2)
				return -1;
			pos[found] = i;
			xinv[found] = xi;
			found++;
		}
	}
	if (found != deg)
		return -1;

	// omega(x) = synd(x) * lam(x) mod x^16
	uint8 omega[kRsRoots];
	for (int i = 0; i < kRsRoots; i++) {
		uint8 acc = 0;
		int top = i < deg ? i : deg;
		for (int j = 0; j <= top; j++)
			acc ^= GfMul(lam[j], synd[i - j]);
		omega[i] = acc;
	}

	for (int k = 0; k < found; k++) {
		uint8 xi = xinv[k];
		uint8 num = 0;
		uint8 xp = 1;
		for (int i = 0; i < kRsRoots; i++) {
			num ^= GfMul(omega[i], xp);
			xp = GfMul(xp, xi);
		}
		// Formal derivative: in GF(2^m) only the odd terms survive.
		uint8 den = 0;
		xp = 1;
		for (int i = 1; i <= deg; i++) {
			if (i & 1)
				den ^= GfMul(lam[i], xp);
			xp = GfMul(xp, xi);
		}
		if (den == 0)
			return -1;
		uint8 mag = GfMul(GfMul(num, GfInv(den)), GfInv(xi));
		r[pos[k]] ^= mag;
	}

	// A corrected block must have vanishing syndromes; anything else is a
	// misdecode and gets rejected rather than passed on as data.
	for (int j = 0; j < kRsRoots; j++) {
		uint8 s = 0;
		for (int i = 0; i < kRsBytes; i++)
			s = (uint8)(GfMul(s, gExp[j]) ^ r[i]);
		if (s)
			return -1;
	}
	return deg;
}

int
Parity(int v)
{
	int p = 0;
	while (v) {
		p ^= v & 1;
		v >>= 1;
	}
	return p;
}

} // namespace

DabMsc::DabMsc()
	:
	fConfigured(false),
	fHardDecision(false),
	fQuantizedDecision(false),
	fSubBits(0),
	fUsefulBits(0),
	fMotherBits(0),
	fL1(0),
	fL2(0),
	fPattern1(NULL),
	fPattern2(NULL),
	fCifIndex(0),
	fSynced(false),
	fRsFailStreak(0),
	fBranchPhase(0),
	fDeintIndex(0),
	fBlockFill(0),
	fSkip(0)
{
	_BuildTrellis();
	fMetric.resize(64);
	fNextMetric.resize(64);
	fBlock.resize(kRsBlock);
}

void
DabMsc::_BuildTrellis()
{
	for (int s = 0; s < 64; s++) {
		for (int u = 0; u < 2; u++) {
			int r = (u << 6) | s;
			uint8 mask = 0;
			for (int j = 0; j < 4; j++) {
				if (Parity(r & kDabGenerators[j]))
					mask |= (uint8)(1 << j);
			}
			fOutMask[s][u] = mask;
		}
		fPred[s][0] = 2 * (s & 31);
		fPred[s][1] = 2 * (s & 31) + 1;
		fInBit[s] = (s >> 5) & 1;
	}
}

bool
DabMsc::_PlanFor(int kbps, int level, bool optionB, int* l1, int* l2,
	int* consumed) const
{
	int n1;
	int n2;
	if (optionB) {
		static const int kPairB[4][2] = {{10, 9}, {6, 5}, {4, 3}, {2, 1}};
		n1 = kPairB[level - 1][0];
		n2 = kPairB[level - 1][1];
		*l1 = 24 * kbps / 32 - 3;
		*l2 = 3;
	} else {
		switch (level) {
			case 1:
				*l1 = 6 * kbps / 8 - 3; *l2 = 3; n1 = 24; n2 = 23; break;
			case 2:
				*l1 = 2 * kbps / 8 - 3; *l2 = 4 * kbps / 8 + 3;
				n1 = 14; n2 = 13; break;
			case 3:
				*l1 = 6 * kbps / 8 - 3; *l2 = 3; n1 = 8; n2 = 7; break;
			default:
				*l1 = 4 * kbps / 8 - 3; *l2 = 2 * kbps / 8 + 3;
				n1 = 3; n2 = 2; break;
		}
	}
	if (*l1 < 0 || *l2 < 0)
		return false;

	int ones1 = 0;
	int ones2 = 0;
	int onesTail = 0;
	for (int k = 0; k < 32; k++) {
		ones1 += kDabPCodes[n1 - 1][k];
		ones2 += kDabPCodes[n2 - 1][k];
	}
	for (int k = 0; k < 24; k++)
		onesTail += kDabPiTail[k];

	*consumed = *l1 * 4 * ones1 + *l2 * 4 * ones2 + onesTail;
	return true;
}

bool
DabMsc::Configure(const config& c)
{
	fConfigured = false;
	if (c.sizeCu <= 0 || c.startCu < 0 || c.startCu + c.sizeCu > 864)
		return false;
	if (c.protectionLevel < 1 || c.protectionLevel > 4)
		return false;

	fSubBits = c.sizeCu * 64;
	int step = c.eepOptionB ? 32 : 8;
	int bestKbps = 0;
	int l1 = 0;
	int l2 = 0;
	for (int kbps = step; kbps <= 2048; kbps += step) {
		int tl1;
		int tl2;
		int used;
		if (!_PlanFor(kbps, c.protectionLevel, c.eepOptionB, &tl1, &tl2, &used))
			continue;
		// Two independent constraints, and only the right profile meets both:
		// the soft bits consumed must be exactly the subchannel size, and the
		// mother code must yield exactly the useful bit count.
		if (used == fSubBits && (tl1 + tl2) * 32 == kbps * 24) {
			bestKbps = kbps;
			l1 = tl1;
			l2 = tl2;
			break;
		}
	}
	if (bestKbps == 0)
		return false;

	int n1;
	int n2;
	if (c.eepOptionB) {
		static const int kPairB[4][2] = {{10, 9}, {6, 5}, {4, 3}, {2, 1}};
		n1 = kPairB[c.protectionLevel - 1][0];
		n2 = kPairB[c.protectionLevel - 1][1];
	} else {
		static const int kPairA[4][2] = {{24, 23}, {14, 13}, {8, 7}, {3, 2}};
		n1 = kPairA[c.protectionLevel - 1][0];
		n2 = kPairA[c.protectionLevel - 1][1];
	}

	fConfig = c;
	fL1 = l1;
	fL2 = l2;
	fPattern1 = kDabPCodes[n1 - 1];
	fPattern2 = kDabPCodes[n2 - 1];
	fMotherBits = (l1 + l2) * 128 + 24;
	fUsefulBits = (l1 + l2) * 32;
	if (fUsefulBits % 8 != 0)
		return false;

	fRing.assign((size_t)kTimeDepth * fSubBits, 0.0f);
	fInter.assign(fSubBits, 0.0f);
	fCoded.assign(fMotherBits, 0.0f);
	fBits.assign(fMotherBits / 4, 0);
	fMetricPick.assign((size_t)(fMotherBits / 4), 0);
	fBytes.assign((size_t)fUsefulBits / 8, 0);

	// Energy dispersal: 9-bit register of ones, feedback from taps 8 and 4,
	// restarted for every logical frame.
	fPrbs.assign(fUsefulBits, 0);
	int reg[9];
	for (int i = 0; i < 9; i++)
		reg[i] = 1;
	for (int i = 0; i < fUsefulBits; i++) {
		int b = reg[8] ^ reg[4];
		for (int j = 8; j > 0; j--)
			reg[j] = reg[j - 1];
		reg[0] = b;
		fPrbs[i] = (uint8)b;
	}

	// Forney deinterleaver: branch i is delayed by (I-1-i)*M branch visits.
	fBranch.assign(12, std::vector<uint8>());
	fBranchPos.assign(12, 0);
	for (int i = 0; i < 12; i++)
		fBranch[i].assign((size_t)(11 - i) * 17, 0);

	fStats = stats();
	fStats.bitrateKbps = bestKbps;
	fCifIndex = 0;
	fSynced = false;
	fBranchPhase = 0;
	fDeintIndex = 0;
	fBlockFill = 0;
	fSkip = kDeintLatency;
	fRaw.clear();
	fPackets.clear();
	fConfigured = true;
	return true;
}

void
DabMsc::Reset()
{
	if (!fConfigured)
		return;
	config c = fConfig;
	Configure(c);
}

void
DabMsc::_Depuncture(const float* in)
{
	memset(&fCoded[0], 0, fMotherBits * sizeof(float));
	int src = 0;
	int dst = 0;
	const int counts[2] = { fL1, fL2 };
	const int8* patterns[2] = { fPattern1, fPattern2 };

	for (int p = 0; p < 2; p++) {
		for (int b = 0; b < counts[p]; b++) {
			for (int sub = 0; sub < 4; sub++) {
				for (int k = 0; k < 32; k++) {
					if (patterns[p][k])
						fCoded[dst] = in[src++];
					dst++;
				}
			}
		}
	}
	for (int k = 0; k < 24; k++) {
		if (kDabPiTail[k])
			fCoded[dst] = in[src++];
		dst++;
	}
}

void
DabMsc::_Viterbi(const float* soft, int nbits)
{
	const float kNeg = -1e18f;
	for (int s = 0; s < 64; s++)
		fMetric[s] = kNeg;
	fMetric[0] = 0.0f;

#if !defined(__SSE2__)
	uint8 g0idx[32], g1idx[32];
	for (int low = 0; low < 32; low++) {
		g0idx[low] = fOutMask[2 * low][0];
		g1idx[low] = fOutMask[2 * low + 1][0];
	}
#endif
	for (int t = 0; t < nbits; t++) {
		const float* r = soft + 4 * t;
		float branch[16];
		float sum = r[0] + r[1] + r[2] + r[3];
		for (int mask = 0; mask < 16; mask++) {
			float negative = 0.0f;
			if (mask & 1) negative += r[0];
			if (mask & 2) negative += r[1];
			if (mask & 4) negative += r[2];
			if (mask & 8) negative += r[3];
			branch[mask] = sum - 2.0f * negative;
		}
		uint64 picks = 0;
#if defined(__SSE2__)
		// The 32 low states use only four distinct four-lane branch layouts.
		// Build two from the eight required correlations and obtain the other
		// two by reversal, instead of doing 32 indexed scalar loads in the ACS
		// loop on every decoded bit.
		__m128 branchVec[8];
		branchVec[0] = _mm_setr_ps(branch[0], branch[9], branch[4], branch[13]);
		branchVec[1] = _mm_setr_ps(branch[11], branch[2], branch[15], branch[6]);
		branchVec[2] = branchVec[1];
		branchVec[3] = branchVec[0];
		branchVec[4] = _mm_shuffle_ps(branchVec[1], branchVec[1],
			_MM_SHUFFLE(0, 1, 2, 3));
		branchVec[5] = _mm_shuffle_ps(branchVec[0], branchVec[0],
			_MM_SHUFFLE(0, 1, 2, 3));
		branchVec[6] = branchVec[5];
		branchVec[7] = branchVec[4];
		// Four low states per iteration. The predecessors of lows
		// [low..low+3] are the eight contiguous metrics fMetric[2*low..+7],
		// deinterleaved into the even (p0) and odd (p1) lanes; the low and
		// low+32 destinations are add/sub of the same branch metric. The
		// operand order (max(x1, x0), cmpgt(x1, x0)) reproduces the scalar
		// `x1 > x0 ? x1 : x0` tie-break exactly, so the result is bit-identical.
		for (int low = 0; low < 32; low += 4) {
			const float* m = &fMetric[(size_t)(2 * low)];
			__m128 a = _mm_loadu_ps(m);
			__m128 b = _mm_loadu_ps(m + 4);
			__m128 ev = _mm_shuffle_ps(a, b, _MM_SHUFFLE(2, 0, 2, 0));
			__m128 od = _mm_shuffle_ps(a, b, _MM_SHUFFLE(3, 1, 3, 1));
			__m128 b0 = branchVec[low >> 2];
			__m128 lo0 = _mm_add_ps(ev, b0);
			// The two predecessor states differ in the oldest register bit.
			// All four DAB generators contain that bit, so its encoder word is
			// the complement and its correlation is exactly -b0.
			__m128 lo1 = _mm_sub_ps(od, b0);
			_mm_storeu_ps(&fNextMetric[(size_t)low], _mm_max_ps(lo1, lo0));
			picks |= (uint64)(unsigned)_mm_movemask_ps(_mm_cmpgt_ps(lo1, lo0))
				<< low;
			__m128 hi0 = _mm_sub_ps(ev, b0);
			__m128 hi1 = _mm_add_ps(od, b0);
			_mm_storeu_ps(&fNextMetric[(size_t)(low + 32)],
				_mm_max_ps(hi1, hi0));
			picks |= (uint64)(unsigned)_mm_movemask_ps(_mm_cmpgt_ps(hi1, hi0))
				<< (low + 32);
		}
#else
		for (int low = 0; low < 32; low++) {
			float b0 = branch[g0idx[low]];
			float b1 = branch[g1idx[low]];
			float pm0 = fMetric[2 * low];
			float pm1 = fMetric[2 * low + 1];
			float lo0 = pm0 + b0;
			float lo1 = pm1 + b1;
			fNextMetric[low] = lo1 > lo0 ? lo1 : lo0;
			picks |= (uint64)(lo1 > lo0) << low;

			// All four generators include the newest input bit, so input 1
			// complements the four encoder outputs and negates the branch
			// correlation. The high destination shares the predecessors.
			float hi0 = pm0 - b0;
			float hi1 = pm1 - b1;
			fNextMetric[low + 32] = hi1 > hi0 ? hi1 : hi0;
			picks |= (uint64)(hi1 > hi0) << (low + 32);
		}
#endif
		fMetricPick[(size_t)t] = picks;
		// A CIF path metric stays below 1e6, so per-bit renormalisation only
		// doubled the hot-loop memory traffic. Copying is enough here.
		fMetric.swap(fNextMetric);
	}

	int s = 0;
	for (int t = nbits - 1; t >= 0; t--) {
		fBits[t] = (uint8)((s >> 5) & 1);
		int pick = (int)((fMetricPick[(size_t)t] >> s) & 1);
		s = fPred[s][pick];
	}
}

void
DabMsc::_ViterbiHard(const float* soft, int nbits)
{
	int metric[64], next[64];
	for (int s = 0; s < 64; s++) metric[s] = -1000000000;
	metric[0] = 0;
	for (int t = 0; t < nbits; t++) {
		const float* r = soft + 4 * t;
		int branch[16];
		int bits[4];
		for (int i = 0; i < 4; i++) bits[i] = r[i] >= 0.0f ? 1 : -1;
		for (int mask = 0; mask < 16; mask++) {
			int score = 0;
			for (int i = 0; i < 4; i++)
				score += ((mask >> i) & 1) ? -bits[i] : bits[i];
			branch[mask] = score;
		}
		uint64 picks = 0;
		for (int low = 0; low < 32; low++) {
			int p0 = 2 * low, p1 = p0 + 1;
			int m0 = metric[p0] + branch[fOutMask[p0][0]];
			int m1 = metric[p1] + branch[fOutMask[p1][0]];
			bool c = m1 > m0; next[low] = c ? m1 : m0;
			if (c) picks |= (uint64)1 << low;
			m0 = metric[p0] - branch[fOutMask[p0][0]];
			m1 = metric[p1] - branch[fOutMask[p1][0]];
			c = m1 > m0; next[low + 32] = c ? m1 : m0;
			if (c) picks |= (uint64)1 << (low + 32);
		}
		fMetricPick[(size_t)t] = picks;
		for (int s = 0; s < 64; s++) metric[s] = next[s];
	}
	int s = 0;
	for (int t = nbits - 1; t >= 0; t--) {
		fBits[t] = (uint8)((s >> 5) & 1);
		int pick = (int)((fMetricPick[(size_t)t] >> s) & 1);
		s = fPred[s][pick];
	}
}

void
DabMsc::_ViterbiQuantized(const float* soft, int nbits)
{
	// 8-bit soft metrics. Kept because they hold RS on mYTN (verified: 100% on
	// a clean capture, 164/165 at the margin - unlike hard decision at 0), but
	// left disabled: an int16 SSE version that runs eight states wide was
	// measured slower than the four-wide float _Viterbi on the Atom once the
	// per-step renormalisation and int16 deinterleave were paid for.
	int metric[64], next[64];
	for (int s = 0; s < 64; s++) metric[s] = -1000000000;
	metric[0] = 0;
	for (int t = 0; t < nbits; t++) {
		const float* r = soft + 4 * t;
		int q[4];
		for (int i = 0; i < 4; i++) {
			int v = (int)(r[i] * 32.0f);
			if (v < -127) v = -127;
			if (v > 127) v = 127;
			q[i] = v;
		}
		int branch[16];
		for (int mask = 0; mask < 16; mask++) {
			int score = 0;
			for (int i = 0; i < 4; i++)
				score += ((mask >> i) & 1) ? -q[i] : q[i];
			branch[mask] = score;
		}
		uint64 picks = 0;
		for (int low = 0; low < 32; low++) {
			int p0 = 2 * low, p1 = p0 + 1;
			int m0 = metric[p0] + branch[fOutMask[p0][0]];
			int m1 = metric[p1] + branch[fOutMask[p1][0]];
			bool c = m1 > m0; next[low] = c ? m1 : m0;
			if (c) picks |= (uint64)1 << low;
			m0 = metric[p0] - branch[fOutMask[p0][0]];
			m1 = metric[p1] - branch[fOutMask[p1][0]];
			c = m1 > m0; next[low + 32] = c ? m1 : m0;
			if (c) picks |= (uint64)1 << (low + 32);
		}
		fMetricPick[(size_t)t] = picks;
		for (int s = 0; s < 64; s++) metric[s] = next[s];
	}
	int s = 0;
	for (int t = nbits - 1; t >= 0; t--) {
		fBits[t] = (uint8)((s >> 5) & 1);
		int pick = (int)((fMetricPick[(size_t)t] >> s) & 1);
		s = fPred[s][pick];
	}
}

void
DabMsc::_Deinterleave(uint8 b)
{
	// Calls always consume a contiguous byte stream. Keeping the branch phase
	// explicitly avoids an integer remainder in this per-byte hot path.
	int i = fBranchPhase;
	if (++fBranchPhase == 12)
		fBranchPhase = 0;
	uint8 out;
	if (fBranch[i].empty())
		out = b;
	else {
		out = fBranch[i][fBranchPos[i]];
		fBranch[i][fBranchPos[i]] = b;
		fBranchPos[i] = (fBranchPos[i] + 1) % (int)fBranch[i].size();
	}

	if (fSkip > 0) {
		fSkip--;
		return;
	}
	fBlock[fBlockFill++] = out;
	if (fBlockFill == kRsBlock) {
		fBlockFill = 0;
		fStats.blocksTried++;
		int err = RsDecode(&fBlock[0]);
		if (err >= 0) {
			fStats.rsOk++;
			fStats.rsCorrected += (uint64)err;
			fRsFailStreak = 0;
			fPackets.insert(fPackets.end(), fBlock.begin(),
				fBlock.begin() + kTsPacket);
		} else if (++fRsFailStreak >= kRsResyncFailures) {
			// The byte boundary drifted (a CIF discontinuity at a live USB
			// read boundary shifts the 204-byte alignment; fSynced would
			// otherwise stay wrong forever). Re-search it, keeping the time
			// interleaver (fRing/fCifIndex), which self-heals over 16 CIFs.
			fRsFailStreak = 0;
			fSynced = false;
			fStats.synced = false;
			fRaw.clear();
			fBlockFill = 0;
			fBranchPhase = 0;
			fSkip = kDeintLatency;
			for (size_t q = 0; q < fBranch.size(); q++) {
				std::fill(fBranch[q].begin(), fBranch[q].end(), 0);
				fBranchPos[q] = 0;
			}
		}
	}
}

void
DabMsc::_PushBytes(const uint8* bytes, int count)
{
	if (!fSynced) {
		fRaw.insert(fRaw.end(), bytes, bytes + count);
		// The sync byte is the one thing the interleaver leaves alone: it goes
		// through branch 0, whose delay is zero, so it stays 204 apart even in
		// the interleaved stream. That gives both the RS block boundary and the
		// branch phase, since branch = position mod 12 and 204 mod 12 == 0.
		const int kProbe = 5;
		size_t limit = (size_t)kRsBlock * 12;
		if (fRaw.size() < limit + (size_t)kRsBlock * kProbe)
			return;
		for (size_t p = 0; p < limit; p += 12) {
			bool ok = true;
			for (int k = 0; k < kProbe; k++) {
				if (fRaw[p + (size_t)k * kRsBlock] != 0x47) {
					ok = false;
					break;
				}
			}
			if (!ok)
				continue;
			fSynced = true;
			fStats.synced = true;
			fDeintIndex = 0;
			fBranchPhase = 0;
			for (size_t q = p; q < fRaw.size(); q++)
				_Deinterleave(fRaw[q]);
			fDeintIndex += (int)(fRaw.size() - p);
			fRaw.clear();
			return;
		}
		// Keep the tail only; a boundary further along will be found later.
		if (fRaw.size() > limit + (size_t)kRsBlock * (kProbe + 2))
			fRaw.erase(fRaw.begin(), fRaw.begin() + kRsBlock * 12);
		return;
	}
	for (int i = 0; i < count; i++)
		_Deinterleave(bytes[i]);
	fDeintIndex += count;
}

int
DabMsc::PushCif(const float* cifSoft)
{
	if (!fConfigured)
		return 0;

	size_t slot = (size_t)(fCifIndex % kTimeDepth) * fSubBits;
	memcpy(&fRing[slot], cifSoft + (size_t)fConfig.startCu * 64,
		fSubBits * sizeof(float));
	fCifIndex++;
	fStats.cifsIn++;
	if (fCifIndex < (uint64)kTimeDepth)
		return 0;

	// Output logical frame n = newest - 15 takes bit i from CIF n + D[i mod 16].
	uint64 base = fCifIndex - kTimeDepth;
	for (int i = 0; i < fSubBits; i++) {
		uint64 src = base + (uint64)kDelay[i & 15];
		fInter[i] = fRing[(size_t)(src % kTimeDepth) * fSubBits + i];
	}

	_Depuncture(&fInter[0]);
	if (fHardDecision)
		_ViterbiHard(&fCoded[0], fMotherBits / 4);
	else if (fQuantizedDecision)
		_ViterbiQuantized(&fCoded[0], fMotherBits / 4);
	else
		_Viterbi(&fCoded[0], fMotherBits / 4);
	fStats.framesOut++;

	int before = (int)fPackets.size();
	int nbytes = fUsefulBits / 8;
	// Pack eight decoded bits at a time. The old bit loop performed a divide
	// and remainder for every bit and allocated/zeroed a vector for every CIF.
	// fUsefulBits is byte-aligned for every valid EEP profile.
	for (int i = 0; i < nbytes; i++) {
		int b = i * 8;
		uint8 value = 0;
		value |= (uint8)((fBits[b] ^ fPrbs[b]) << 7);
		value |= (uint8)((fBits[b + 1] ^ fPrbs[b + 1]) << 6);
		value |= (uint8)((fBits[b + 2] ^ fPrbs[b + 2]) << 5);
		value |= (uint8)((fBits[b + 3] ^ fPrbs[b + 3]) << 4);
		value |= (uint8)((fBits[b + 4] ^ fPrbs[b + 4]) << 3);
		value |= (uint8)((fBits[b + 5] ^ fPrbs[b + 5]) << 2);
		value |= (uint8)((fBits[b + 6] ^ fPrbs[b + 6]) << 1);
		value |= (uint8)(fBits[b + 7] ^ fPrbs[b + 7]);
		fBytes[(size_t)i] = value;
	}
	_PushBytes(&fBytes[0], nbytes);
	return ((int)fPackets.size() - before) / kTsPacket;
}

void
DabMsc::SkipCifs(uint64 count)
{
	if (!fConfigured || count == 0)
		return;
	// A missing span invalidates every delayed input it crosses. Zero the
	// corresponding ring slots; a full interleaver depth clears the ring.
	if (count >= (uint64)kTimeDepth) {
		memset(&fRing[0], 0, fRing.size() * sizeof(float));
	} else {
		for (uint64 n = 0; n < count; n++) {
			size_t slot = (size_t)((fCifIndex + n) % kTimeDepth) * fSubBits;
			memset(&fRing[slot], 0, fSubBits * sizeof(float));
		}
	}
	fCifIndex += count;
	fStats.cifsIn += count;

	// TS byte continuity is gone, so reacquire the outer RS boundary. Do not
	// reset the MSC configuration or time-interleaver phase.
	fSynced = false;
	fStats.synced = false;
	fRaw.clear();
	fPackets.clear();
	fBranchPhase = 0;
	fDeintIndex = 0;
	fBlockFill = 0;
	fSkip = kDeintLatency;
	for (size_t i = 0; i < fBranch.size(); i++) {
		std::fill(fBranch[i].begin(), fBranch[i].end(), 0);
		fBranchPos[i] = 0;
	}
}
