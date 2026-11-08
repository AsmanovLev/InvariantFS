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

static unsigned long long parse_u64_c23(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f' || *s == '\v') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') { s++; }
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16; s += 2;
    } else if ((base == 0 || base == 2) && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
        base = 2; s += 2;
    } else if (base == 0) {
        base = (s[0] == '0') ? 8 : 10;
    }
    unsigned long long acc = 0;
    int any = 0;
    for (;; s++) {
        int c = (unsigned char)*s;
        int dig;
        if (c >= '0' && c <= '9') dig = c - '0';
        else if (c >= 'a' && c <= 'z') dig = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') dig = c - 'A' + 10;
        else break;
        if (dig >= base) break;
        acc = acc * (unsigned long long)base + (unsigned long long)dig;
        any = 1;
    }
    if (endptr) *endptr = (char *)(any ? s : nptr);
    return neg ? (unsigned long long)(-(long long)acc) : acc;
}

long int __isoc23_strtol(const char *nptr, char **endptr, int base) __attribute__((weak));
long int __isoc23_strtol(const char *nptr, char **endptr, int base) {
    return (long int)parse_u64_c23(nptr, endptr, base);
}

unsigned long int __isoc23_strtoul(const char *nptr, char **endptr, int base) __attribute__((weak));
unsigned long int __isoc23_strtoul(const char *nptr, char **endptr, int base) {
    return (unsigned long int)parse_u64_c23(nptr, endptr, base);
}

long long int __isoc23_strtoll(const char *nptr, char **endptr, int base) __attribute__((weak));
long long int __isoc23_strtoll(const char *nptr, char **endptr, int base) {
    return (long long int)parse_u64_c23(nptr, endptr, base);
}

unsigned long long int __isoc23_strtoull(const char *nptr, char **endptr, int base) __attribute__((weak));
unsigned long long int __isoc23_strtoull(const char *nptr, char **endptr, int base) {
    return parse_u64_c23(nptr, endptr, base);
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

