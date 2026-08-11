/*
 * Just enough of Haiku's <SupportDefs.h> and <OS.h> for the portable half of
 * this project to compile on macOS.
 *
 * The DSP and DAB code (Dsp, Demodulator, DabSync, DabFic, DabMsc, DabTables,
 * DmbAudio) is plain C++ that happens to spell its integers the BeAPI way.
 * Rather than edit every one of those files - which would mean maintaining two
 * versions of code whose correctness took a long time to establish - this
 * header is put first on the include path for the macOS build, so the sources
 * are shared byte for byte with the Haiku app.
 *
 * Nothing here emulates the BeAPI. The parts of the app that genuinely need it
 * - BWindow, BSoundPlayer, BLocker - are not built on macOS; the macOS target
 * supplies its own device, audio and UI layers.
 */
#ifndef RSDR_MACOS_SUPPORTDEFS_H
#define RSDR_MACOS_SUPPORTDEFS_H

#include <stdint.h>
#include <stddef.h>
#include <sys/time.h>

typedef int8_t		int8;
typedef uint8_t		uint8;
typedef int16_t		int16;
typedef uint16_t	uint16;
typedef int32_t		int32;
typedef uint32_t	uint32;
typedef int64_t		int64;
typedef uint64_t	uint64;

typedef unsigned char	uchar;
typedef unsigned int	uint;

typedef int32_t		status_t;
typedef int64_t		bigtime_t;
typedef int32_t		thread_id;
typedef int32_t		sem_id;

#define B_OK			((status_t)0)
#define B_ERROR			((status_t)-1)
#define B_BAD_VALUE		((status_t)-2147483647)
#define B_TIMED_OUT		((status_t)-2147483639)
#define B_NO_MEMORY		((status_t)-2147483648LL)
#define B_DEV_NOT_READY	((status_t)-2147454955)
#define B_BAD_INDEX		((status_t)-2147483643)

// Microseconds since an arbitrary epoch, which is all the callers need.
static inline bigtime_t
system_time(void)
{
	struct timeval tv;
	gettimeofday(&tv, 0);
	return (bigtime_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

#endif // RSDR_MACOS_SUPPORTDEFS_H
