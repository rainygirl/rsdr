/*
 * Tables shared by the FIC and MSC decoders (ETSI EN 300 401).
 *
 * These lived inside DabFic.cpp until the MSC decoder needed the same
 * puncturing vectors. One copy, because a transcription slip in the second
 * copy would show up as "the FIC decodes and the MSC does not", which is
 * exactly the symptom that costs days to track down.
 */
#ifndef RSDR_DAB_TABLES_H
#define RSDR_DAB_TABLES_H

#include <SupportDefs.h>

// Puncturing vectors PI_1..PI_24 (table 29), 32 entries each, indexed 0..23
// for PI_1..PI_24.
extern const int8 kDabPCodes[24][32];
// The 24-bit tail pattern PI_X that terminates every coded block.
extern const int8 kDabPiTail[24];
// Rate-1/4 mother code, K = 7: 0133, 0171, 0145, 0133. The fourth genuinely
// repeats the first.
extern const int kDabGenerators[4];

#endif // RSDR_DAB_TABLES_H
