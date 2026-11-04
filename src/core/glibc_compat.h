#pragma once
/*
 * glibc_compat.h -- Compatibility shim for glibc >= 2.38
 *
 * When compiling with glibc 2.38+ and _GNU_SOURCE, glibc defaults to redirecting
 * strtol/strtoul/strtoull/fscanf to their C23 (__isoc23_*) counterparts.
 * When binaries are run under Alpine Linux via gcompat (musl compatibility layer),
 * gcompat does not export the __isoc23_* symbols, causing relocation failure.
 *
 * Pre-including this header undefines __GLIBC_USE_ISOC23 and __GLIBC_USE_C23_STRTOL
 * so that standard, widely-compatible C99/POSIX symbol versions are emitted.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if defined(__GLIBC__) || defined(__linux__)
#include <features.h>
#undef __GLIBC_USE_ISOC23
#define __GLIBC_USE_ISOC23 0
#undef __GLIBC_USE_C23_STRTOL
#define __GLIBC_USE_C23_STRTOL 0
#endif
