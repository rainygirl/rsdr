#include "DabFic.h"

#include <cstring>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

#include "DabTables.h"

namespace {

// The tables themselves now live in DabTables.cpp, shared with the MSC
// decoder; these are just the local names the code below already used.
const int8 (&kPCodes)[24][32] = kDabPCodes;
const int8 (&kPiTail)[24] = kDabPiTail;
const int (&kGenerators)[4] = kDabGenerators;


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

std::string
LabelFrom(const uint8* data, int length)
{
	std::string s((const char*)data, length);
	while (!s.empty() && (s[s.size() - 1] == ' ' || s[s.size() - 1] == '\0'))
		s.erase(s.size() - 1);
	// Labels are Latin-1 or one of DAB's own character sets; strip anything
	// that would corrupt the display rather than trying to convert.
	for (size_t i = 0; i < s.size(); i++) {
		unsigned char c = (unsigned char)s[i];
		if (c < 0x20 || c > 0x7E)
			s[i] = '?';
	}
	return s;
}

} // namespace

DabFic::DabFic()
	:
	fEnsembleId(0),
	fFibsOk(0),
	fFibsTried(0)
{
	for (int i = 0; i < 4096; i++) {
		fPacketSubchannel[i] = -1;
		fPacketType[i] = -1;
	}
	for (int s = 0; s < 64; s++) {
		for (int u = 0; u < 2; u++) {
			int r = (u << 6) | s;
			uint8 mask = 0;
			for (int j = 0; j < 4; j++) {
				if (Parity(r & kGenerators[j]))
					mask |= (uint8)(1 << j);
			}
			fOutMask[s][u] = mask;
		}
		// Each state has exactly two predecessors, and both reach it with the
		// same input bit - which is what makes the add-compare-select cheap.
		fPred[s][0] = 2 * (s & 31);
		fPred[s][1] = 2 * (s & 31) + 1;
		fInBit[s] = (s >> 5) & 1;
	}

	// Energy dispersal: 9-bit register of ones, feedback from taps 8 and 4.
	fPrbs.resize(kDataBits);
	int reg[9];
	for (int i = 0; i < 9; i++)
		reg[i] = 1;
	for (int i = 0; i < kDataBits; i++) {
		int b = reg[8] ^ reg[4];
		for (int j = 8; j > 0; j--)
			reg[j] = reg[j - 1];
		reg[0] = b;
		fPrbs[i] = (uint8)b;
	}

	fCoded.resize(kCodedBits);
	fDecoded.resize(kCodedBits / 4);
	fMetric.resize(64);
	fNextMetric.resize(64);
	fMetricPick.resize((size_t)(kCodedBits / 4));
}

void
DabFic::Reset()
{
	fEnsembleId = 0;
	fEnsembleLabel.clear();
	fServices.clear();
	fSubchannels.clear();
	for (int i = 0; i < 4096; i++) {
		fPacketSubchannel[i] = -1;
		fPacketType[i] = -1;
	}
	fFibsOk = 0;
	fFibsTried = 0;
}

void
DabFic::_Depuncture(const float* in, float* out) const
{
	// 21 blocks with PI_16, 3 with PI_15, then a 24-bit tail with PI_X.
	// Consumes exactly 21*96 + 3*92 + 12 = 2304 soft bits, which is the check
	// that this is right.
	memset(out, 0, kCodedBits * sizeof(float));
	int src = 0;
	int dst = 0;
	const int counts[2] = { 21, 3 };
	const int8* patterns[2] = { kPCodes[15], kPCodes[14] };

	for (int p = 0; p < 2; p++) {
		for (int b = 0; b < counts[p]; b++) {
			for (int sub = 0; sub < 4; sub++) {
				for (int k = 0; k < 32; k++) {
					if (patterns[p][k])
						out[dst] = in[src++];
					dst++;
				}
			}
		}
	}
	for (int k = 0; k < 24; k++) {
		if (kPiTail[k])
			out[dst] = in[src++];
		dst++;
	}
}

void
DabFic::_Viterbi(const float* soft, int nbits, uint8* bits)
{
	const float kNeg = -1e18f;
	for (int s = 0; s < 64; s++)
		fMetric[s] = kNeg;
	fMetric[0] = 0.0f;

	// See DabMsc::_Viterbi for the vectorised add-compare-select. Same K=7
	// trellis and layout, so the same SSE2 kernel and the same RS oracle apply.
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
		for (int low = 0; low < 32; low += 4) {
			const float* m = &fMetric[(size_t)(2 * low)];
			__m128 a = _mm_loadu_ps(m);
			__m128 b = _mm_loadu_ps(m + 4);
			__m128 ev = _mm_shuffle_ps(a, b, _MM_SHUFFLE(2, 0, 2, 0));
			__m128 od = _mm_shuffle_ps(a, b, _MM_SHUFFLE(3, 1, 3, 1));
			__m128 b0 = branchVec[low >> 2];
			__m128 lo0 = _mm_add_ps(ev, b0);
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
			float hi0 = pm0 - b0;
			float hi1 = pm1 - b1;
			fNextMetric[low + 32] = hi1 > hi0 ? hi1 : hi0;
			picks |= (uint64)(hi1 > hi0) << (low + 32);
		}
#endif
		fMetricPick[(size_t)t] = picks;
		fMetric.swap(fNextMetric);
	}

	// The encoder is flushed with six zeros, so the path ends in state 0.
	int s = 0;
	for (int t = nbits - 1; t >= 0; t--) {
		bits[t] = (uint8)((s >> 5) & 1);
		int pick = (int)((fMetricPick[(size_t)t] >> s) & 1);
		s = fPred[s][pick];
	}
}

static bool
CrcOk(const uint8* bits)
{
	// EN 300 401 CRC-16 over the first 240 bits, remainder complemented and
	// compared against the last 16.
	uint16 reg = 0xFFFF;
	for (int i = 0; i < 240; i++) {
		int top = (reg >> 15) & 1;
		reg = (uint16)(reg << 1);
		if (top ^ bits[i])
			reg ^= 0x1021;
	}
	reg = (uint16)(reg ^ 0xFFFF);
	uint16 got = 0;
	for (int i = 240; i < 256; i++)
		got = (uint16)((got << 1) | bits[i]);
	return reg == got;
}

int
DabFic::DecodeFrame(const float* softBits)
{
	int good = 0;
	uint8 fib[30];

	for (int b = 0; b < kBlocksPerFrame; b++) {
		_Depuncture(softBits + b * kSoftBitsPerBlock, &fCoded[0]);
		_Viterbi(&fCoded[0], kCodedBits / 4, &fDecoded[0]);

		for (int i = 0; i < kDataBits; i++)
			fDecoded[i] ^= fPrbs[i];

		for (int f = 0; f < 3; f++) {
			const uint8* bits = &fDecoded[f * kFibBits];
			fFibsTried++;
			if (!CrcOk(bits))
				continue;
			fFibsOk++;
			good++;
			memset(fib, 0, sizeof(fib));
			for (int i = 0; i < 240; i++) {
				if (bits[i])
					fib[i / 8] |= (uint8)(0x80 >> (i % 8));
			}
			_ParseFib(fib);
		}
	}
	return good;
}

void
DabFic::_ParseFib(const uint8* fib)
{
	int pos = 0;
	while (pos < 30) {
		uint8 header = fib[pos];
		if (header == 0xFF)
			break;					// end marker / padding
		int type = header >> 5;
		int length = header & 0x1F;
		pos++;
		if (length == 0 || pos + length > 30)
			break;

		if (type == 0)
			_ParseFig0(fib + pos, length);
		else if (type == 1)
			_ParseFig1(fib + pos, length);
		pos += length;
	}
}

DabFic::service*
DabFic::_ServiceFor(uint32 id)
{
	for (size_t i = 0; i < fServices.size(); i++) {
		if (fServices[i].id == id)
			return &fServices[i];
	}
	if (fServices.size() >= 32)
		return NULL;
	service s;
	s.id = id;
	fServices.push_back(s);
	return &fServices[fServices.size() - 1];
}

void
DabFic::_ParseFig0(const uint8* body, int length)
{
	int ext = body[0] & 0x1F;
	int cn = (body[0] >> 7) & 1;
	int pd = (body[0] >> 5) & 1;
	if (cn)
		return;						// next-configuration, not current
	const uint8* d = body + 1;
	int n = length - 1;

	if (ext == 0 && n >= 2) {
		fEnsembleId = (uint32)((d[0] << 8) | d[1]);

	} else if (ext == 1) {
		int i = 0;
		while (i + 3 <= n) {
			subchannel sub;
			sub.id = (d[i] >> 2) & 0x3F;
			sub.startCu = ((d[i] & 0x03) << 8) | d[i + 1];
			if (((d[i + 2] >> 7) & 1) == 0) {
				sub.shortForm = true;
				i += 3;
			} else {
				if (i + 4 > n)
					break;
				sub.eepOptionB = ((d[i + 2] >> 4) & 0x07) != 0;
				sub.protectionLevel = ((d[i + 2] >> 2) & 0x03) + 1;
				sub.sizeCu = ((d[i + 2] & 0x03) << 8) | d[i + 3];
				i += 4;
			}
			bool found = false;
			for (size_t k = 0; k < fSubchannels.size(); k++) {
				if (fSubchannels[k].id == sub.id) {
					fSubchannels[k] = sub;
					found = true;
					break;
				}
			}
			if (!found && fSubchannels.size() < 64)
				fSubchannels.push_back(sub);
		}

	} else if (ext == 2) {
		int i = 0;
		while (i < n) {
			uint32 sid;
			int ncomp;
			if (pd) {
				if (i + 5 > n)
					break;
				sid = ((uint32)d[i] << 24) | ((uint32)d[i + 1] << 16)
					| ((uint32)d[i + 2] << 8) | d[i + 3];
				ncomp = d[i + 4] & 0x0F;
				i += 5;
			} else {
				if (i + 3 > n)
					break;
				sid = (uint32)((d[i] << 8) | d[i + 1]);
				ncomp = d[i + 2] & 0x0F;
				i += 3;
			}
			service* svc = _ServiceFor(sid);
			for (int c = 0; c < ncomp && i + 2 <= n; c++) {
				int tmid = (d[i] >> 6) & 0x03;
				if (svc != NULL && (tmid == 0 || tmid == 1)) {
					// Only the primary component gets recorded: a T-DMB
					// service is one stream, and listing its secondaries adds
					// noise to the display without adding information.
					bool primary = ((d[i + 1] >> 1) & 1) != 0;
					if (primary || svc->subchannel < 0) {
						svc->isAudio = (tmid == 0);
						svc->type = d[i] & 0x3F;
						svc->subchannel = (d[i + 1] >> 2) & 0x3F;
						svc->isPrimary = primary;
						svc->conditionalAccess = (d[i + 1] & 1) != 0;
					}
				} else if (svc != NULL && tmid == 3) {
					// Packet-mode components carry a 12-bit SCId here. FIG 0/3
					// supplies the DSCTy and physical subchannel separately.
					bool primary = ((d[i + 1] >> 1) & 1) != 0;
					int scid = ((d[i] & 0x3F) << 6)
						| ((d[i + 1] >> 2) & 0x3F);
					if (primary || svc->scid < 0) {
						svc->isAudio = false;
						svc->scid = scid;
						svc->subchannel = fPacketSubchannel[scid];
						svc->type = fPacketType[scid];
						svc->isPrimary = primary;
						svc->conditionalAccess = (d[i + 1] & 1) != 0;
					}
				}
				i += 2;
			}
		}

	} else if (ext == 3) {
		// EN 300 401 FIG 0/3, one packet-mode component per 5-byte
		// field, followed by an optional 2-byte CAOrg. Layout:
		// SCId:12, Rfa:3, CAOrg flag:1, DG:1, Rfu:1, DSCTy:6,
		// SubChId:6, packet address:10, [CAOrg:16].
		int i = 0;
		while (i + 5 <= n) {
			int scid = (d[i] << 4) | (d[i + 1] >> 4);
			bool caOrg = (d[i + 1] & 0x01) != 0;
			int dscty = d[i + 2] & 0x3F;
			int subchannel = (d[i + 3] >> 2) & 0x3F;
			fPacketSubchannel[scid] = subchannel;
			fPacketType[scid] = dscty;
			for (size_t k = 0; k < fServices.size(); k++) {
				if (fServices[k].scid == scid) {
					fServices[k].subchannel = subchannel;
					fServices[k].type = dscty;
				}
			}
			i += caOrg ? 7 : 5;
		}
	}
}

void
DabFic::_ParseFig1(const uint8* body, int length)
{
	int ext = body[0] & 0x07;
	const uint8* d = body + 1;
	int n = length - 1;

	if (ext == 0 && n >= 18) {
		fEnsembleLabel = LabelFrom(d + 2, 16);
	} else if (ext == 1 && n >= 18) {
		uint32 sid = (uint32)((d[0] << 8) | d[1]);
		service* s = _ServiceFor(sid);
		if (s != NULL)
			s->label = LabelFrom(d + 2, 16);
	} else if (ext == 5 && n >= 20) {
		uint32 sid = ((uint32)d[0] << 24) | ((uint32)d[1] << 16)
			| ((uint32)d[2] << 8) | d[3];
		service* s = _ServiceFor(sid);
		if (s != NULL)
			s->label = LabelFrom(d + 4, 16);
	}
}
