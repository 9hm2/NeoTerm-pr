/* uKernel <linux/signal.h>. On bionic, libc's <signal.h> pulls the real
 * kernel-uapi <linux/signal.h> for __sighandler_t/sigset_t/stack_t. The old
 * empty stub (safe on glibc, which never pulls kernel uapi from libc) breaks
 * bionic, so chain to the toolchain's real header. uKernel's fake task/signal
 * types live in <linux/sched.h>, not here. */
#ifndef _UK_LINUX_SIGNAL_H
#define _UK_LINUX_SIGNAL_H
#if defined(__BIONIC__)
#include_next <linux/signal.h>
#else
#include <linux/types.h>
#include <linux/sched.h>
#endif
#endif
