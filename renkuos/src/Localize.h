/*
 * Tiny UI-string localiser. Follows the system's preferred language for a small
 * fixed set of labels; anything else, or an unsupported language, stays English.
 * Supported: Korean, Japanese, Italian, Turkish, French.
 */
#ifndef RSDR_LOCALIZE_H
#define RSDR_LOCALIZE_H

// Detect the system language once, before building the UI.
void TrInit();

// Return the localised UTF-8 string for a known English key, or the key itself.
const char* Tr(const char* english);

#endif
