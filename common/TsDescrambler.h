/* Optional DVB-CSA transport-stream descrambler for user-supplied keys. */
#ifndef RSDR_TS_DESCRAMBLER_H
#define RSDR_TS_DESCRAMBLER_H

#include <SupportDefs.h>

#include <string>

class TsDescrambler {
public:
	struct stats {
		uint64 clearPackets;
		uint64 decryptedEven;
		uint64 decryptedOdd;
		uint64 missingKey;
		uint64 invalidPackets;
		uint64 scrambledPesStarts;
		uint64 validPesStarts;
		stats() : clearPackets(0), decryptedEven(0), decryptedOdd(0),
			missingKey(0), invalidPackets(0), scrambledPesStarts(0),
			validPesStarts(0) {}
	};

			TsDescrambler();
			~TsDescrambler();

	bool	Load(std::string& error);
	bool	SetEvenKey(const std::string& hex, std::string& error);
	bool	SetOddKey(const std::string& hex, std::string& error);
	void	ClearKeys();
	// Decrypts one 188-byte TS packet in place and clears its scrambling bits.
	// An optional PID restricts which elementary stream is touched.
	bool	ProcessPacket(uint8* packet, int onlyPid = -1);
	bool	SelfTest(std::string& error);
	bool	HasKey() const { return fHaveEven || fHaveOdd; }
	bool	IsLoaded() const { return fLibrary != NULL; }
	const stats& Stats() const { return fStats; }

private:
	typedef void* (*alloc_fn)();
	typedef void (*free_fn)(void*);
	typedef void (*set_fn)(const uint8*, void*);
	typedef void (*crypt_fn)(const void*, uint8*, unsigned int);

	bool	_SetKey(const std::string& hex, void*& key, bool& present,
			std::string& error);
	static bool _ParseKey(const std::string& hex, uint8 key[8]);

	void*	fLibrary;
	void*	fEvenKey;
	void*	fOddKey;
	bool	fHaveEven;
	bool	fHaveOdd;
	alloc_fn fAlloc;
	free_fn fFree;
	set_fn	fSet;
	crypt_fn fDecrypt;
	crypt_fn fEncrypt;
	stats	fStats;
};

#endif
