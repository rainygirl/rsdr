/* T-DMB MPEG-2 TS demultiplexer and MPEG-4 ER-BSAC audio decoder. */
#ifndef RSDR_DMB_AUDIO_H
#define RSDR_DMB_AUDIO_H

#include <SupportDefs.h>

#include <map>
#include <vector>

struct bsac_decoder;

class DmbAudio {
public:
	struct audio_anchor {
		size_t sampleOffset;
		uint64 pts90k;
		audio_anchor() : sampleOffset(0), pts90k(0) {}
	};
	struct video_unit {
		std::vector<uint8> data;
		uint64 pts90k;
		bool hasPts;
		video_unit() : pts90k(0), hasPts(false) {}
	};
	struct stats {
		uint64 packets;
		uint64 accessUnits;
		uint64 decodedUnits;
		uint64 errors;
		int audioPid;
		int sampleRate;
		int channels;
		double maxUnitRms;
		double maxDifferenceRms;
		float maxBoundaryJump;
		uint64 loudUnits;
		uint64 concealedUnits;
		uint64 slHeader5;
		uint64 slHeader9;
		uint64 slHeaderOther;
		stats() : packets(0), accessUnits(0), decodedUnits(0), errors(0),
			audioPid(-1), sampleRate(0), channels(0), maxUnitRms(0),
			maxDifferenceRms(0), maxBoundaryJump(0), loudUnits(0),
			concealedUnits(0), slHeader5(0), slHeader9(0), slHeaderOther(0) {}
	};

				DmbAudio();
				~DmbAudio();
	void		Reset();
	// Appends mono float PCM and returns the number of frames appended.
	size_t		Feed(const uint8* packets, size_t bytes,
				std::vector<float>& pcm);
	const stats& Stats() const { return fStats; }

	// Video, when the service carries it. T-DMB puts H.264 in a second SL
	// stream alongside the audio; the configuration below is the
	// AVCDecoderConfigurationRecord ("avcC") the object descriptor stream
	// carries, and the access units are the H.264 payloads with the PES and SL
	// headers already removed, in the length-prefixed form that record
	// describes. Nothing here decodes video - that belongs to the platform.
	const std::vector<uint8>& VideoConfig() const { return fVideoConfig; }
	const std::vector<uint8>& AudioSlConfig() const { return fSlConfig; }
	const std::vector<uint8>& VideoSlConfig() const { return fVideoSlConfig; }
				int		VideoPid() const { return fVideoPid; }
	// Moves the oldest complete access unit out, if there is one.
				bool	TakeVideoUnit(std::vector<uint8>& out);
				bool	TakeVideoUnit(video_unit& out);
	// PTS anchors produced since the last call. sampleOffset refers to the PCM
	// vector passed to Feed(), so the sink can bind that burst to the 90 kHz
	// MPEG clock without changing the shared decoder API.
				void	TakeAudioAnchors(std::vector<audio_anchor>& out);
	// Diagnostics: which ES_IDs the PMT mapped to which PIDs, and which ES the
	// object descriptor stream said carries video.
	const std::map<int, int>& EsPidMap() const { return fEsPid; }
				int		VideoEsId() const { return fVideoEsId; }
				int		AudioEsId() const { return fAudioEsId; }

private:
	struct section_state {
		std::vector<uint8> data;
		size_t expected;
		section_state() : expected(0) {}
	};

	void		_PushSection(int pid, const uint8* data, size_t size,
				bool start);
	void		_ParseSection(int pid, const uint8* data, size_t size);
	void		_ParsePat(const uint8* data, size_t size);
	void		_ParsePmt(const uint8* data, size_t size);
	void		_ParseOd(const uint8* data, size_t size);
	void		_PushPes(const uint8* data, size_t size, bool start,
				std::vector<float>& pcm);
	void		_PushVideoPes(const uint8* data, size_t size, bool start);
	void		_FinishVideoPes();
	void		_DecodePes(std::vector<float>& pcm);
	void		_TryConfigure();
	void		_Discontinuity(int pid);
	void		_ResetAudioDecoder();
	static bool	_ReadLength(const uint8* data, size_t size, size_t& pos,
				size_t& length);
	static bool	_PesPts(const std::vector<uint8>& pes, uint64& pts90k);

	int			fPmtPid;
	int			fAudioEsId;
	int			fVideoEsId;
	int			fVideoPid;
	std::vector<uint8> fVideoConfig;
	std::vector<uint8> fVideoSlConfig;
	std::vector<uint8> fVideoPes;
	size_t		fVideoPesExpected;
	std::vector<video_unit> fVideoUnits;
	std::vector<audio_anchor> fAudioAnchors;
	std::map<int, int> fEsPid;
	std::map<int, section_state> fSections;
	std::map<int, int> fContinuity;
	std::vector<uint8> fConfig;
	std::vector<uint8> fSlConfig;
	std::vector<uint8> fPes;
	size_t		fPesExpected;
			bsac_decoder*	fDecoder;
			float		fLastOutputSample;
			bool		fHaveLastOutputSample;
			size_t		fConcealFrames;
			uint64		fConcealPts90k;
			bool		fHaveConcealPts;
			int			fConcealFollowingUnits;
	stats		fStats;
};

#endif
