#include "Localize.h"

#include <cstring>

#include <LocaleRoster.h>
#include <Message.h>
#include <String.h>

namespace {

// 0 = English (fallback), then Korean, Japanese, Italian, Turkish, French.
int gLang = 0;

struct Entry {
	const char*	en;
	const char*	t[5];	// ko, ja, it, tr, fr - all UTF-8.
};

const Entry kTable[] = {
	{ "Presets", { "\xED\x94\x84\xEB\xA6\xAC\xEC\x85\x8B", // 프리셋 (Presets)
		"\xE3\x83\x97\xE3\x83\xAA\xE3\x82\xBB\xE3\x83\x83\xE3\x83\x88", // プリセット
		"Preset", "\xC3\x96n Ayarlar", "Pr\xC3\xA9r\xC3\xA9glages" } },
	{ "Volume", { "\xEB\xB3\xBC\xEB\xA5\xA8",             // 볼륨
		"\xE9\x9F\xB3\xE9\x87\x8F",                       // 音量
		"Volume", "Ses", "Volume" } },
	{ "Squelch", { "\xEC\x8A\xA4\xEC\xBC\x88\xEC\xB9\x98", // 스켈치
		"\xE3\x82\xB9\xE3\x82\xB1\xE3\x83\xAB\xE3\x83\x81", // スケルチ
		"Squelch", "Susturma", "Silencieux" } },
	{ "Signal", { "\xEC\x8B\xA0\xED\x98\xB8",             // 신호
		"\xE4\xBF\xA1\xE5\x8F\xB7",                       // 信号
		"Segnale", "Sinyal", "Signal" } },
	{ "idle", { "\xEB\x8C\x80\xEA\xB8\xB0",               // 대기
		"\xE5\xBE\x85\xE6\xA9\x9F",                       // 待機
		"inattivo", "bo\xC5\x9Fta", "inactif" } },
	{ "Stereo", { "\xEC\x8A\xA4\xED\x85\x8C\xEB\xA0\x88\xEC\x98\xA4", // 스테레오
		"\xE3\x82\xB9\xE3\x83\x86\xE3\x83\xAC\xE3\x82\xAA", // ステレオ
		"Stereo", "Stereo", "St\xC3\xA9r\xC3\xA9o" } },
};

}	// namespace

void
TrInit()
{
	gLang = 0;
	BMessage languages;
	if (BLocaleRoster::Default()->GetPreferredLanguages(&languages) != B_OK)
		return;
	BString code;
	if (languages.FindString("language", 0, &code) != B_OK)
		return;
	if (code.StartsWith("ko"))
		gLang = 1;
	else if (code.StartsWith("ja"))
		gLang = 2;
	else if (code.StartsWith("it"))
		gLang = 3;
	else if (code.StartsWith("tr"))
		gLang = 4;
	else if (code.StartsWith("fr"))
		gLang = 5;
}

const char*
Tr(const char* english)
{
	if (gLang == 0 || english == NULL)
		return english;
	for (size_t i = 0; i < sizeof(kTable) / sizeof(kTable[0]); i++) {
		if (strcmp(kTable[i].en, english) == 0)
			return kTable[i].t[gLang - 1];
	}
	return english;
}
