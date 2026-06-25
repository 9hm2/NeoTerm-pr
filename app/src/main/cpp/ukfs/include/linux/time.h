/* uKernel <linux/time.h>. On bionic, libc <sys/time.h> pulls the real kernel
 * uapi <linux/time.h> for struct timeval/timespec/timezone; the old stub (which
 * redefined struct timezone and lacked struct timeval) breaks it. Chain to the
 * real header and add only uKernel's kernel-internal extras. */
#ifndef _UK_LINUX_TIME_H
#define _UK_LINUX_TIME_H
#if defined(__BIONIC__)
#include_next <linux/time.h>          /* struct timeval/timespec/timezone, CLOCK_* */
#include <time.h>                     /* libc struct tm (gmtime_r target in vfs.c) */
#include <linux/types.h>
#include <linux/time64.h>
#else
#include <linux/types.h>
#include <linux/time64.h>
struct timezone { int tz_minuteswest; int tz_dsttime; };
#endif
extern struct timezone sys_tz;
#define SECS_PER_DAY	(24 * 60 * 60)
#define days_in_month(m) (0)
/* time64_to_tm: real impl in vfs.c (libc gmtime_r); FAT/exfat time encoding uses it. */
void time64_to_tm(time64_t totalsecs, int offset, void *result);
extern time64_t mktime64(unsigned int year, unsigned int mon, unsigned int day,
			 unsigned int hour, unsigned int min, unsigned int sec);
#endif
