/* uKernel Wi-Fi — glibc-ABI shims for a driver .so cross-built under a glibc
 * distro (Kali) and dlopen'd by the BIONIC daemon.
 *
 * glibc 2.38+ redirects scanf/strtol family to versioned __isoc23_* symbols
 * (C23 ABI; _GNU_SOURCE pulls _ISOC2X_SOURCE, so even -std=gnu11 keeps them).
 * bionic has no __isoc23_* — dlopen would fail to resolve them. Provide thin
 * forwarders to the plain functions (which bionic does have). Only the entries a
 * vendor Wi-Fi driver actually uses, plus the obvious siblings for headroom.
 *
 * (The __aarch64_*_acq_rel outline-atomic helpers a driver may also reference are
 * NOT here — those are compiler codegen, removed by building the driver with
 * -mno-outline-atomics; see wifi/build-driver.sh.) */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

int __isoc23_sscanf(const char *s, const char *fmt, ...)
{ va_list a; va_start(a, fmt); int r = vsscanf(s, fmt, a); va_end(a); return r; }
int __isoc23_scanf(const char *fmt, ...)
{ va_list a; va_start(a, fmt); int r = vscanf(fmt, a); va_end(a); return r; }
int __isoc23_fscanf(FILE *fp, const char *fmt, ...)
{ va_list a; va_start(a, fmt); int r = vfscanf(fp, fmt, a); va_end(a); return r; }
int __isoc23_vsscanf(const char *s, const char *fmt, va_list a) { return vsscanf(s, fmt, a); }

long          __isoc23_strtol(const char *p, char **e, int b)   { return strtol(p, e, b); }
unsigned long __isoc23_strtoul(const char *p, char **e, int b)  { return strtoul(p, e, b); }
long long     __isoc23_strtoll(const char *p, char **e, int b)  { return strtoll(p, e, b); }
unsigned long long __isoc23_strtoull(const char *p, char **e, int b) { return strtoull(p, e, b); }
