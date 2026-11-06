/*
 * glibc_compat.c -- Runtime compatibility shims for musl/gcompat
 *
 * When static libraries (such as libfuse3.a built on modern glibc systems) are linked
 * into binaries that run on musl-based distributions (e.g. Alpine Linux via gcompat),
 * some glibc-specific internal symbols like strerrordesc_np, fcntl64, __isoc23_strtol,
 * and __isoc23_strtoul are unresolved because gcompat does not export them.
 *
 * Providing weak definitions here satisfies the linker/loader without conflicting
 * with real glibc when running on a glibc system.
 */
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>

const char *strerrordesc_np(int errnum) __attribute__((weak));
const char *strerrordesc_np(int errnum) {
    return strerror(errnum);
}

#ifndef fcntl64
int fcntl64(int fd, int cmd, ...) __attribute__((weak));
int fcntl64(int fd, int cmd, ...) {
    va_list va;
    va_start(va, cmd);
    void *arg = va_arg(va, void *);
    va_end(va);
    return fcntl(fd, cmd, arg);
}
#endif

long int __isoc23_strtol(const char *nptr, char **endptr, int base) __attribute__((weak));
long int __isoc23_strtol(const char *nptr, char **endptr, int base) {
    return strtol(nptr, endptr, base);
}

unsigned long int __isoc23_strtoul(const char *nptr, char **endptr, int base) __attribute__((weak));
unsigned long int __isoc23_strtoul(const char *nptr, char **endptr, int base) {
    return strtoul(nptr, endptr, base);
}

long long int __isoc23_strtoll(const char *nptr, char **endptr, int base) __attribute__((weak));
long long int __isoc23_strtoll(const char *nptr, char **endptr, int base) {
    return strtoll(nptr, endptr, base);
}

unsigned long long int __isoc23_strtoull(const char *nptr, char **endptr, int base) __attribute__((weak));
unsigned long long int __isoc23_strtoull(const char *nptr, char **endptr, int base) {
    return strtoull(nptr, endptr, base);
}

#include <stdio.h>

int __isoc23_fscanf(FILE *stream, const char *format, ...) __attribute__((weak));
int __isoc23_fscanf(FILE *stream, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int ret = vfscanf(stream, format, args);
    va_end(args);
    return ret;
}

int __isoc23_vfscanf(FILE *stream, const char *format, va_list arg) __attribute__((weak));
int __isoc23_vfscanf(FILE *stream, const char *format, va_list arg) {
    return vfscanf(stream, format, arg);
}

int __isoc23_sscanf(const char *str, const char *format, ...) __attribute__((weak));
int __isoc23_sscanf(const char *str, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int ret = vsscanf(str, format, args);
    va_end(args);
    return ret;
}

int __isoc23_vsscanf(const char *str, const char *format, va_list arg) __attribute__((weak));
int __isoc23_vsscanf(const char *str, const char *format, va_list arg) {
    return vsscanf(str, format, arg);
}

int __isoc23_scanf(const char *format, ...) __attribute__((weak));
int __isoc23_scanf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int ret = vscanf(format, args);
    va_end(args);
    return ret;
}

int __isoc23_vscanf(const char *format, va_list arg) __attribute__((weak));
int __isoc23_vscanf(const char *format, va_list arg) {
    return vscanf(format, arg);
}

