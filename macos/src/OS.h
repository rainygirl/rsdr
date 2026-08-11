/*
 * Stand-in for Haiku's <OS.h> on macOS.
 *
 * Only DabProbe includes it, and only for system_time(); everything else it
 * would provide (threads, semaphores, ports) belongs to the BeAPI layers that
 * the macOS target does not build. See SupportDefs.h in this directory for why
 * the shared sources are not edited instead.
 */
#ifndef RSDR_MACOS_OS_H
#define RSDR_MACOS_OS_H

#include "SupportDefs.h"

#endif // RSDR_MACOS_OS_H
