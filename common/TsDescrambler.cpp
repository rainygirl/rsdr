#include "TsDescrambler.h"

#include <dlfcn.h>

#include <cstring>

TsDescrambler::TsDescrambler()
	: fLibrary(NULL), fEvenKey(NULL), fOddKey(NULL), fHaveEven(false),
	  fHaveOdd(false), fAlloc(NULL), fFree(NULL), fSet(NULL), fDecrypt(NULL),
	  fEncrypt(NULL)
{
}

TsDescrambler::~TsDescrambler()
{
	if (fFree != NULL) {
		if (fEvenKey != NULL)
			fFree(fEvenKey);
		if (fOddKey != NULL)
			fFree(fOddKey);
	}
	if (fLibrary != NULL)
		dlclose(fLibrary);
}

bool
TsDescrambler::Load(std::string& error)
{
	if (fLibrary != NULL)
		return true;
	const char* paths[] = {
		"/opt/homebrew/opt/libdvbcsa/lib/libdvbcsa.dylib",
		"/usr/local/opt/libdvbcsa/lib/libdvbcsa.dylib",
		"libdvbcsa.dylib", "libdvbcsa.so.1", "libdvbcsa.so"
	};
	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		fLibrary = dlopen(paths[i], RTLD_NOW | RTLD_LOCAL);
		if (fLibrary != NULL)
			break;
	}
	if (fLibrary == NULL) {
		error = "libdvbcsa is not installed (brew install libdvbcsa)";
		return false;
	}
	fAlloc = (alloc_fn)dlsym(fLibrary, "dvbcsa_key_alloc");
	fFree = (free_fn)dlsym(fLibrary, "dvbcsa_key_free");
	fSet = (set_fn)dlsym(fLibrary, "dvbcsa_key_set");
	fDecrypt = (crypt_fn)dlsym(fLibrary, "dvbcsa_decrypt");
	fEncrypt = (crypt_fn)dlsym(fLibrary, "dvbcsa_encrypt");
	if (fAlloc == NULL || fFree == NULL || fSet == NULL || fDecrypt == NULL
		|| fEncrypt == NULL) {
		error = "installed libdvbcsa has an incompatible API";
		dlclose(fLibrary);
		fLibrary = NULL;
		return false;
	}
	fEvenKey = fAlloc();
	fOddKey = fAlloc();
	if (fEvenKey == NULL || fOddKey == NULL) {
		error = "libdvbcsa could not allocate key contexts";
		return false;
	}
	return true;
}

bool
TsDescrambler::_ParseKey(const std::string& text, uint8 key[8])
{
	std::string hex;
	for (size_t i = 0; i < text.size(); i++) {
		char c = text[i];
		if (c == ':' || c == '-' || c == ' ')
			continue;
		hex += c;
	}
	if (hex.size() != 12 && hex.size() != 16)
		return false;
	uint8 parsed[8];
	int bytes = (int)hex.size() / 2;
	for (int i = 0; i < bytes; i++) {
		int value = 0;
		for (int n = 0; n < 2; n++) {
			char c = hex[i * 2 + n];
			int digit = c >= '0' && c <= '9' ? c - '0'
				: c >= 'a' && c <= 'f' ? c - 'a' + 10
				: c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
			if (digit < 0)
				return false;
			value = (value << 4) | digit;
		}
		parsed[i] = (uint8)value;
	}
	if (bytes == 6) {
		key[0] = parsed[0];
		key[1] = parsed[1];
		key[2] = parsed[2];
		key[3] = (uint8)(parsed[0] + parsed[1] + parsed[2]);
		key[4] = parsed[3];
		key[5] = parsed[4];
		key[6] = parsed[5];
		key[7] = (uint8)(parsed[3] + parsed[4] + parsed[5]);
	} else {
		memcpy(key, parsed, 8);
	}
	return true;
}

bool
TsDescrambler::_SetKey(const std::string& hex, void*& context, bool& present,
	std::string& error)
{
	if (!Load(error))
		return false;
	uint8 key[8];
	if (!_ParseKey(hex, key)) {
		error = "a DVB-CSA control word must contain 12 or 16 hex digits";
		return false;
	}
	fSet(key, context);
	present = true;
	return true;
}

bool
TsDescrambler::SetEvenKey(const std::string& hex, std::string& error)
{
	return _SetKey(hex, fEvenKey, fHaveEven, error);
}

bool
TsDescrambler::SetOddKey(const std::string& hex, std::string& error)
{
	return _SetKey(hex, fOddKey, fHaveOdd, error);
}

void
TsDescrambler::ClearKeys()
{
	fHaveEven = false;
	fHaveOdd = false;
	fStats = stats();
}

bool
TsDescrambler::ProcessPacket(uint8* packet, int onlyPid)
{
	if (packet == NULL || packet[0] != 0x47) {
		fStats.invalidPackets++;
		return false;
	}
	int pid = ((packet[1] & 0x1f) << 8) | packet[2];
	int scrambling = packet[3] >> 6;
	if (scrambling == 0 || (onlyPid >= 0 && pid != onlyPid)) {
		fStats.clearPackets++;
		return true;
	}
	if (scrambling != 2 && scrambling != 3) {
		fStats.invalidPackets++;
		return false;
	}
	void* key = scrambling == 2 ? fEvenKey : fOddKey;
	bool present = scrambling == 2 ? fHaveEven : fHaveOdd;
	if (!present || fDecrypt == NULL) {
		fStats.missingKey++;
		return false;
	}
	int afc = (packet[3] >> 4) & 3;
	if (afc != 1 && afc != 3) {
		// Adaptation-only packets carry no encrypted payload. Clearing the bits
		// makes downstream MPEG parsers accept them without touching data.
		packet[3] &= 0x3f;
		return true;
	}
	size_t pos = 4;
	if (afc == 3) {
		pos += 1 + packet[4];
		if (pos > 188) {
			fStats.invalidPackets++;
			return false;
		}
	}
	if (pos < 188)
		fDecrypt(key, packet + pos, (unsigned int)(188 - pos));
	if ((packet[1] & 0x40) != 0 && pos + 3 <= 188) {
		fStats.scrambledPesStarts++;
		if (packet[pos] == 0 && packet[pos + 1] == 0 && packet[pos + 2] == 1)
			fStats.validPesStarts++;
	}
	packet[3] &= 0x3f;
	if (scrambling == 2)
		fStats.decryptedEven++;
	else
		fStats.decryptedOdd++;
	return true;
}

bool
TsDescrambler::SelfTest(std::string& error)
{
	if (!Load(error))
		return false;
	const uint8 key[8] = { 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00 };
	const uint8 expected[16] = {
		0x2d, 0x0a, 0x47, 0x20, 0x18, 0x11, 0x9c, 0x8a,
		0xd1, 0x2a, 0x65, 0x6b, 0x89, 0xe4, 0x35, 0x2b
	};
	uint8 data[184];
	uint8 original[184];
	for (int i = 0; i < 184; i++)
		data[i] = original[i] = (uint8)i;
	fSet(key, fEvenKey);
	// Published libdvbcsa vector 2: bytes 00..b7 are the encrypted input;
	// decryption begins with the bytes below for this control word.
	fDecrypt(fEvenKey, data, sizeof(data));
	if (memcmp(data, expected, sizeof(expected)) != 0) {
		error = "DVB-CSA decryption test vector failed";
		return false;
	}
	fEncrypt(fEvenKey, data, sizeof(data));
	if (memcmp(data, original, sizeof(data)) != 0) {
		error = "DVB-CSA encrypt round-trip failed";
		return false;
	}
	uint8 packet[188];
	packet[0] = 0x47;
	packet[1] = 0x41;
	packet[2] = 0x23;
	packet[3] = 0xd0; // odd-key scrambled, payload only, continuity 0
	packet[4] = packet[5] = 0;
	packet[6] = 1;
	for (int i = 7; i < 188; i++)
		packet[i] = (uint8)i;
	uint8 plain[184];
	memcpy(plain, packet + 4, sizeof(plain));
	fSet(key, fOddKey);
	fEncrypt(fOddKey, packet + 4, sizeof(plain));
	fHaveOdd = true;
	if (!ProcessPacket(packet) || (packet[3] >> 6) != 0
		|| memcmp(packet + 4, plain, sizeof(plain)) != 0) {
		error = "188-byte TS packet descrambling test failed";
		return false;
	}
	return true;
}
