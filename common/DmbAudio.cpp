#include "DmbAudio.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bsac_api.h"

DmbAudio::DmbAudio()
	: fPmtPid(-1), fAudioEsId(-1), fVideoEsId(-1), fVideoPid(-1),
	  fVideoPesExpected(0), fPesExpected(0), fDecoder(NULL),
	  fLastOutputSample(0), fHaveLastOutputSample(false), fConcealFrames(0),
	  fConcealPts90k(0), fHaveConcealPts(false), fConcealFollowingUnits(0)
{
}

DmbAudio::~DmbAudio()
{
	bsac_decoder_destroy(fDecoder);
}

void
DmbAudio::Reset()
{
	bsac_decoder_destroy(fDecoder);
	fDecoder = NULL;
	fPmtPid = -1;
	fVideoEsId = -1;
	fVideoPid = -1;
	fVideoConfig.clear();
	fVideoSlConfig.clear();
	fVideoPes.clear();
	fVideoPesExpected = 0;
	fVideoUnits.clear();
	fAudioAnchors.clear();
	fAudioEsId = -1;
	fEsPid.clear();
	fSections.clear();
	fContinuity.clear();
	fConfig.clear();
	fSlConfig.clear();
	fPes.clear();
	fPesExpected = 0;
	fLastOutputSample = 0;
	fHaveLastOutputSample = false;
	fConcealFrames = 0;
	fHaveConcealPts = false;
	fConcealFollowingUnits = 0;
	fStats = stats();
}

void
DmbAudio::_ResetAudioDecoder()
{
	bsac_decoder_destroy(fDecoder);
	fDecoder = NULL;
	fPes.clear();
	fPesExpected = 0;
	fHaveLastOutputSample = false;
	fConcealFrames = 0;
	fHaveConcealPts = false;
	fConcealFollowingUnits = 0;
	// A continuity gap is detected before its payload is passed to the legacy
	// decoder. Recreate its state here: carrying predictor/arithmetic state
	// across a missing AU produced loud clicks, and malformed concatenated AUs
	// can leave this old decoder in an unbounded decode loop.
	_TryConfigure();
}

void
DmbAudio::_Discontinuity(int pid)
{
	std::map<int, section_state>::iterator section = fSections.find(pid);
	if (section != fSections.end()) {
		section->second.data.clear();
		section->second.expected = 0;
	}
	if (pid == fStats.audioPid) {
		// A missing TS packet discards at least one 1024-sample BSAC access
		// unit. Recreating the decoder prevents predictor corruption, but the old
		// code also threw away the last sample and joined the next AU directly,
		// producing the periodic "ssok" transient heard on DMB (FM never uses
		// this path). Preserve the boundary and let _DecodePes insert a short
		// fade-to-silence/fade-in concealment interval.
		float last = fLastOutputSample;
		bool hadLast = fHaveLastOutputSample;
		_ResetAudioDecoder();
		fLastOutputSample = last;
		fHaveLastOutputSample = hadLast;
		fConcealFrames = 1024;
	}
	if (pid == fVideoPid) {
		fVideoPes.clear();
		fVideoPesExpected = 0;
	}
}

bool
DmbAudio::_ReadLength(const uint8* data, size_t size, size_t& pos,
	size_t& length)
{
	length = 0;
	for (int i = 0; i < 4; i++) {
		if (pos >= size)
			return false;
		uint8 b = data[pos++];
		length = (length << 7) | (b & 0x7f);
		if ((b & 0x80) == 0)
			return pos + length <= size;
	}
	return false;
}

void
DmbAudio::_ParsePat(const uint8* d, size_t n)
{
	if (n < 12 || d[0] != 0x00)
		return;
	size_t end = n >= 4 ? n - 4 : 0;
	for (size_t p = 8; p + 4 <= end; p += 4) {
		int program = (d[p] << 8) | d[p + 1];
		if (program != 0) {
			fPmtPid = ((d[p + 2] & 0x1f) << 8) | d[p + 3];
			break;
		}
	}
}

void
DmbAudio::_ParsePmt(const uint8* d, size_t n)
{
	if (n < 16 || d[0] != 0x02)
		return;
	size_t programInfo = ((d[10] & 0x0f) << 8) | d[11];
	size_t p = 12 + programInfo;
	size_t end = n - 4;
	while (p + 5 <= end) {
		int type = d[p];
		int pid = ((d[p + 1] & 0x1f) << 8) | d[p + 2];
		size_t info = ((d[p + 3] & 0x0f) << 8) | d[p + 4];
		if (p + 5 + info > end)
			break;
		int esId = -1;
		for (size_t q = p + 5; q + 2 <= p + 5 + info;) {
			uint8 tag = d[q++];
			size_t len = d[q++];
			if (q + len > p + 5 + info)
				break;
			if (tag == 0x1e && len >= 2)
				esId = (d[q] << 8) | d[q + 1];
			q += len;
		}
		if (esId >= 0)
			fEsPid[esId] = pid;
		// Object-descriptor streams use table_id 0x05. Listen to every one;
		// the other MPEG-4 systems stream is harmless and simply never matches.
		if (type == 0x13)
			fSections[pid];
		p += 5 + info;
	}
	_TryConfigure();
}

void
DmbAudio::_ParseOd(const uint8* d, size_t n)
{
	if (n < 12 || d[0] != 0x05)
		return;
	// OD commands contain MPEG-4 expandable-length descriptors. Looking for
	// ES_Descriptor tags is safe here because every candidate is bounds-checked
	// and must contain an AAC DecoderConfig plus DecoderSpecificInfo.
	for (size_t p = 8; p + 5 < n - 4; p++) {
		if (d[p] != 0x03)
			continue;
		size_t q = p + 1;
		size_t esLength;
		if (!_ReadLength(d, n - 4, q, esLength) || esLength < 4)
			continue;
		size_t esEnd = q + esLength;
		int esId = (d[q] << 8) | d[q + 1];
		for (size_t c = q + 3; c + 2 < esEnd; c++) {
			if (d[c] != 0x04)
				continue;
			size_t cp = c + 1;
			size_t configLength;
			if (!_ReadLength(d, esEnd, cp, configLength) || configLength < 13)
				continue;
			// 0x40 is MPEG-4 audio, 0x21 is H.264 video. Both arrive in the
			// same object descriptor stream and are told apart only here.
			const uint8 oti = d[cp];
			if (oti != 0x40 && oti != 0x21)
				continue;
			size_t configEnd = cp + configLength;
			for (size_t x = cp + 13; x + 2 <= configEnd;) {
				uint8 tag = d[x++];
				size_t length;
				if (!_ReadLength(d, configEnd, x, length))
					break;
				if (tag == 0x05 && length >= 2) {
					if (oti == 0x40) {
						// Audio object type 22 is ER-BSAC (five high bits).
						if ((d[x] >> 3) == 22) {
							fAudioEsId = esId;
							fConfig.assign(d + x, d + x + length);
						}
					} else if (length >= 7 && d[x] == 0x01) {
						// avcC: version 1, then profile/compat/level and the
						// parameter sets.
						fVideoEsId = esId;
						fVideoConfig.assign(d + x, d + x + length);
					}
				}
				x += length;
			}
			for (size_t x = cp + 13; x + 2 <= esEnd;) {
				uint8 tag = d[x++];
				size_t length;
				if (!_ReadLength(d, esEnd, x, length))
					break;
				if (tag == 0x06) {
					if (esId == fAudioEsId)
						fSlConfig.assign(d + x, d + x + length);
					if (esId == fVideoEsId)
						fVideoSlConfig.assign(d + x, d + x + length);
				}
				x += length;
			}
		}
	}
	_TryConfigure();
}

void
DmbAudio::_TryConfigure()
{
	if (fDecoder != NULL || fAudioEsId < 0 || fConfig.empty())
		return;
	std::map<int, int>::const_iterator it = fEsPid.find(fAudioEsId);
	if (it == fEsPid.end())
		return;
	fDecoder = bsac_decoder_create(&fConfig[0], fConfig.size());
	if (fDecoder == NULL) {
		fStats.errors++;
		return;
	}
	fStats.audioPid = it->second;
	fStats.sampleRate = bsac_decoder_sample_rate(fDecoder);
	fStats.channels = bsac_decoder_channels(fDecoder);
}

bool
DmbAudio::TakeVideoUnit(std::vector<uint8>& out)
{
	if (fVideoUnits.empty())
		return false;
	out.swap(fVideoUnits.front().data);
	fVideoUnits.erase(fVideoUnits.begin());
	return true;
}

bool
DmbAudio::TakeVideoUnit(video_unit& out)
{
	if (fVideoUnits.empty())
		return false;
	out = fVideoUnits.front();
	fVideoUnits.erase(fVideoUnits.begin());
	return true;
}

void
DmbAudio::TakeAudioAnchors(std::vector<audio_anchor>& out)
{
	out.swap(fAudioAnchors);
	fAudioAnchors.clear();
}

bool
DmbAudio::_PesPts(const std::vector<uint8>& pes, uint64& pts90k)
{
	if (pes.size() < 14 || (pes[7] & 0x80) == 0 || pes[8] < 5)
		return false;
	const uint8* p = &pes[9];
	if ((p[0] & 1) == 0 || (p[2] & 1) == 0 || (p[4] & 1) == 0)
		return false;
	pts90k = ((uint64)(p[0] & 0x0e) << 29)
		| ((uint64)p[1] << 22)
		| ((uint64)(p[2] & 0xfe) << 14)
		| ((uint64)p[3] << 7)
		| ((uint64)p[4] >> 1);
	return true;
}

void
DmbAudio::_FinishVideoPes()
{
	// Same shape as the audio path: PES header, then the five-byte SL header
	// this multiplex uses, then the access unit itself.
	if (fVideoPes.size() < 9 || fVideoPes[0] != 0 || fVideoPes[1] != 0
		|| fVideoPes[2] != 1) {
		fVideoPes.clear();
		return;
	}
	size_t payload = 9 + fVideoPes[8];
	const size_t slHeader = 5;
	if (payload + slHeader < fVideoPes.size()
		&& fVideoPes.size() - payload - slHeader <= 1024 * 1024) {
		// Bound the backlog: if nothing is draining these, drop the oldest
		// rather than growing without limit.
		if (fVideoUnits.size() > 60)
			fVideoUnits.erase(fVideoUnits.begin());
		video_unit unit;
		unit.data.assign(fVideoPes.begin() + payload + slHeader, fVideoPes.end());
		unit.hasPts = _PesPts(fVideoPes, unit.pts90k);
		if (!unit.hasPts && slHeader >= 5) {
			// This multiplex's explicit SLConfig is 00 C6 ...: AU-start/end,
			// idle/timestamp flags, 90 kHz timestampResolution and 33 timestamp
			// bits. Its five-byte header leaves two alignment bits below the CTS.
			// Video PES packets do not repeat the PTS in their PES header, so this
			// SL timestamp is their only shared A/V clock.
			const uint8* h = &fVideoPes[payload];
			uint64 raw = ((uint64)h[0] << 32) | ((uint64)h[1] << 24)
				| ((uint64)h[2] << 16) | ((uint64)h[3] << 8) | h[4];
			if ((h[0] & 0xc0) == 0xc0) {
				unit.pts90k = (raw >> 2) & 0x1ffffffffULL;
				unit.hasPts = true;
			}
		}
		fVideoUnits.push_back(unit);
	}
	fVideoPes.clear();
}

void
DmbAudio::_PushVideoPes(const uint8* data, size_t size, bool start)
{
	if (start) {
		if (!fVideoPes.empty() && fVideoPesExpected != 0
			&& fVideoPes.size() == fVideoPesExpected) {
			_FinishVideoPes();
		}
		fVideoPes.clear();
		fVideoPesExpected = 0;
	}
	if (fVideoPes.empty() && !start)
		return;
	fVideoPes.insert(fVideoPes.end(), data, data + size);
	if (fVideoPes.size() > 1024 * 1024) {
		fVideoPes.clear();
		fVideoPesExpected = 0;
		return;
	}
	if (fVideoPesExpected == 0 && fVideoPes.size() >= 6) {
		size_t packetLength = (fVideoPes[4] << 8) | fVideoPes[5];
		if (packetLength != 0)
			fVideoPesExpected = 6 + packetLength;
	}
	if (fVideoPesExpected != 0 && fVideoPes.size() >= fVideoPesExpected) {
		fVideoPes.resize(fVideoPesExpected);
		_FinishVideoPes();
		fVideoPesExpected = 0;
	}
}

void
DmbAudio::_ParseSection(int pid, const uint8* data, size_t size)
{
	if (pid == 0)
		_ParsePat(data, size);
	else if (pid == fPmtPid)
		_ParsePmt(data, size);
	else if (data[0] == 0x05)
		_ParseOd(data, size);
}

void
DmbAudio::_PushSection(int pid, const uint8* data, size_t size, bool start)
{
	section_state& s = fSections[pid];
	if (start) {
		if (size == 0)
			return;
		size_t pointer = data[0];
		if (1 + pointer > size)
			return;
		s.data.clear();
		s.expected = 0;
		data += 1 + pointer;
		size -= 1 + pointer;
	}
	if (s.data.empty() && !start)
		return;
	s.data.insert(s.data.end(), data, data + size);
	if (s.data.size() > 4096) {
		s.data.clear();
		s.expected = 0;
		return;
	}
	if (s.expected == 0 && s.data.size() >= 3)
		s.expected = 3 + (((s.data[1] & 0x0f) << 8) | s.data[2]);
	if (s.expected >= 3 && s.data.size() >= s.expected) {
		_ParseSection(pid, &s.data[0], s.expected);
		s.data.clear();
		s.expected = 0;
	}
}

void
DmbAudio::_DecodePes(std::vector<float>& pcm)
{
	if (fDecoder == NULL || fPes.size() < 14
		|| fPes[0] != 0 || fPes[1] != 0 || fPes[2] != 1 || fPes[3] != 0xfa) {
		return;
	}
	size_t payload = 9 + fPes[8];
	if (payload + 2 >= fPes.size())
		return;
	// The common SL packet header is five bytes, but the broadcaster inserts
	// an additional four-byte timestamp periodically. Feeding those bytes to
	// BSAC corrupts this AU and the following overlap frame. Locate the audio
	// start using BSAC's own 11-bit frameLength, which is the AU byte count.
	size_t slHeader = 0;
	// The explicit SLConfig used by the measured T-DMB services produces
	// 5-byte base headers and 7/9-byte forms when optional timestamp/OCR fields
	// are present. Searching every byte offset is unsafe: AU 724 in the mYTN
	// capture happened to contain an 11-bit value at offset 2 equal to the
	// remaining PES length, so the old "first match" fed three header bytes to
	// BSAC and poisoned that AU plus its overlap frames.
	const size_t candidates[] = { 5, 7, 9 };
	for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
		size_t candidate = candidates[i];
		if (payload + candidate + 2 > fPes.size())
			continue;
		size_t remaining = fPes.size() - payload - candidate;
		size_t declared = ((size_t)fPes[payload + candidate] << 3)
			| (fPes[payload + candidate + 1] >> 5);
		if (declared == remaining) {
			slHeader = candidate;
			break;
		}
	}
	if (slHeader == 0) {
		fStats.errors++;
		return;
	}
	if (slHeader == 5)
		fStats.slHeader5++;
	else if (slHeader == 9)
		fStats.slHeader9++;
	else
		fStats.slHeaderOther++;
	size_t auSize = fPes.size() - payload - slHeader;
	// A PES length is 16-bit. This tighter guard also keeps a damaged header
	// from sending an absurd buffer into the unmaintained BSAC decoder.
	if (auSize == 0 || auSize > 65535) {
		fStats.errors++;
		return;
	}
	fStats.accessUnits++;
	// Legacy Libav's advertised maximum audio output is 192000 bytes.
	int16 samples[96000];
	int count = bsac_decoder_decode(fDecoder, &fPes[payload + slHeader],
		auSize, samples,
		sizeof(samples) / sizeof(samples[0]));
	if (count <= 0) {
		fStats.errors++;
		return;
	}
	int channels = fStats.channels > 0 ? fStats.channels : 2;
	size_t frames = (size_t)count / channels;
	std::vector<float> decoded;
	decoded.reserve(frames);
	double energy = 0.0;
	double differenceEnergy = 0.0;
	float previous = fHaveLastOutputSample ? fLastOutputSample : 0.0f;
	for (size_t i = 0; i < frames; i++) {
		int sum = 0;
		for (int ch = 0; ch < channels; ch++)
			sum += samples[i * channels + ch];
		float sample = (float)sum / (32768.0f * channels);
		decoded.push_back(sample);
		energy += sample * sample;
		float difference = sample - previous;
		differenceEnergy += difference * difference;
		previous = sample;
	}
	bool corrupt = false;
	double unitRms = 0.0;
	double unitDifferenceRms = 0.0;
	float unitBoundary = 0.0f;
	if (frames != 0) {
		unitRms = sqrt(energy / frames);
		unitDifferenceRms = sqrt(differenceEnergy / frames);
		unitBoundary = fHaveLastOutputSample
			? fabsf(decoded[0] - fLastOutputSample) : 0.0f;
		if (unitRms > fStats.maxUnitRms)
			fStats.maxUnitRms = unitRms;
		if (unitDifferenceRms > fStats.maxDifferenceRms)
			fStats.maxDifferenceRms = unitDifferenceRms;
		if (unitBoundary > fStats.maxBoundaryJump)
			fStats.maxBoundaryJump = unitBoundary;
		if (unitRms > 0.20)
			fStats.loudUnits++;
		// The legacy BSAC decoder can report success yet emit a short noise AU.
		// Clean captured ensembles measured max difference RMS 0.068, while the
		// live, audibly damaged path reached 0.261 without tripping the old 0.35
		// threshold. Keep the energy condition so ordinary quiet transients pass.
		corrupt = unitRms > 0.20 && unitDifferenceRms > 0.15;
		if (slHeader != 5 && getenv("RSDR_AUDIO_TRACE") != NULL) {
			fprintf(stderr, "BSAC SL%lu AU %llu pcm %lu rms %.3f diff %.3f "
				"jump %.3f\n", (unsigned long)slHeader,
				(unsigned long long)fStats.accessUnits,
				(unsigned long)pcm.size(), unitRms, unitDifferenceRms,
				unitBoundary);
		}
	}
	uint64 pts90k;
	bool hasPts = _PesPts(fPes, pts90k);
	bool overlapDamage = fConcealFollowingUnits > 0;
	if (overlapDamage)
		fConcealFollowingUnits--;
	if (corrupt || overlapDamage) {
		if (getenv("RSDR_AUDIO_TRACE") != NULL) {
			fprintf(stderr, "BSAC conceal AU %llu pcm %lu rms %.3f diff %.3f "
				"jump %.3f%s%s\n",
				(unsigned long long)fStats.accessUnits,
				(unsigned long)pcm.size(), unitRms, unitDifferenceRms,
				unitBoundary, corrupt ? " corrupt" : "",
				overlapDamage ? " overlap" : "");
		}
		if (fConcealFrames == 0 && hasPts) {
			fConcealPts90k = pts90k;
			fHaveConcealPts = true;
		}
		if (corrupt)
			fConcealFollowingUnits = 1;
		fConcealFrames += frames;
		fStats.concealedUnits++;
		fStats.decodedUnits++;
		return;
	}

	pcm.reserve(pcm.size() + fConcealFrames + frames);
	if (fConcealFrames != 0 && !decoded.empty()) {
		if (fHaveConcealPts) {
			audio_anchor missing;
			missing.sampleOffset = pcm.size();
			missing.pts90k = fConcealPts90k;
			fAudioAnchors.push_back(missing);
		}
		size_t fade = std::min((size_t)64, fConcealFrames / 2);
		float from = fHaveLastOutputSample ? fLastOutputSample : 0.0f;
		for (size_t i = 0; i < fade; i++)
			pcm.push_back(from * (1.0f - (float)(i + 1) / fade));
		for (size_t i = fade; i < fConcealFrames - fade; i++)
			pcm.push_back(0.0f);
		for (size_t i = 0; i < fade; i++)
			pcm.push_back(decoded[0] * (float)(i + 1) / fade);
		fConcealFrames = 0;
		fHaveConcealPts = false;
	}
	if (hasPts) {
		audio_anchor anchor;
		anchor.sampleOffset = pcm.size();
		anchor.pts90k = pts90k;
		fAudioAnchors.push_back(anchor);
	}
	pcm.insert(pcm.end(), decoded.begin(), decoded.end());
	if (!decoded.empty()) {
		fLastOutputSample = decoded.back();
		fHaveLastOutputSample = true;
	}
	fStats.decodedUnits++;
}

void
DmbAudio::_PushPes(const uint8* data, size_t size, bool start,
	std::vector<float>& pcm)
{
	if (start) {
		if (!fPes.empty() && fPesExpected != 0
			&& fPes.size() == fPesExpected) {
			_DecodePes(pcm);
		}
		fPes.clear();
		fPesExpected = 0;
	}
	if (fPes.empty() && !start)
		return;
	fPes.insert(fPes.end(), data, data + size);
	if (fPes.size() > 65541) {
		fPes.clear();
		fPesExpected = 0;
		fStats.errors++;
		return;
	}
	if (fPesExpected == 0 && fPes.size() >= 6) {
		size_t packetLength = (fPes[4] << 8) | fPes[5];
		if (packetLength != 0)
			fPesExpected = 6 + packetLength;
	}
	if (fPesExpected != 0 && fPes.size() >= fPesExpected) {
		fPes.resize(fPesExpected);
		_DecodePes(pcm);
		fPes.clear();
		fPesExpected = 0;
	}
}

size_t
DmbAudio::Feed(const uint8* packets, size_t bytes, std::vector<float>& pcm)
{
	size_t before = pcm.size();
	for (size_t at = 0; at + 188 <= bytes; at += 188) {
		const uint8* t = packets + at;
		if (t[0] != 0x47)
			continue;
		int pid = ((t[1] & 0x1f) << 8) | t[2];
		if (t[1] & 0x80) {
			_Discontinuity(pid);
			fContinuity.erase(pid);
			continue;
		}
		fStats.packets++;
		bool start = (t[1] & 0x40) != 0;
		int control = (t[3] >> 4) & 3;
		if ((control & 1) == 0)
			continue;
		size_t p = 4;
		bool indicatedGap = false;
		if (control & 2) {
			if (t[4] > 0 && (t[5] & 0x80) != 0)
				indicatedGap = true;
			p += 1 + t[4];
			if (p > 188)
				continue;
		}

		int cc = t[3] & 0x0f;
		std::map<int, int>::iterator previous = fContinuity.find(pid);
		bool gap = indicatedGap;
		if (previous != fContinuity.end()) {
			if (cc == previous->second)
				continue;	// retransmitted packet, never append it twice
			if (cc != ((previous->second + 1) & 0x0f))
				gap = true;
		}
		fContinuity[pid] = cc;
		if (gap) {
			_Discontinuity(pid);
			// The current payload is useful only if it starts a fresh section or
			// PES. Otherwise it is the tail of the unit whose middle was lost.
			if (!start)
				continue;
		}
		if (pid == 0 || pid == fPmtPid || fSections.find(pid) != fSections.end())
			_PushSection(pid, t + p, 188 - p, start);
		if (pid == fStats.audioPid)
			_PushPes(t + p, 188 - p, start, pcm);
		if (fVideoPid < 0 && fVideoEsId >= 0) {
			std::map<int, int>::const_iterator v = fEsPid.find(fVideoEsId);
			if (v != fEsPid.end())
				fVideoPid = v->second;
		}
		if (pid == fVideoPid)
			_PushVideoPes(t + p, 188 - p, start);
	}
	return pcm.size() - before;
}
