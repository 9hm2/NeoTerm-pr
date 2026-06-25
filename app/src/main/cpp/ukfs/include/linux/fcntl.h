/* uKernel <linux/fcntl.h>. bionic's libc <fcntl.h> includes the real kernel
 * uapi <linux/fcntl.h> for O_RDWR/O_CREAT/... — the old stub shadowed it and
 * defined only O_RDONLY, so the O_* constants vanished. Chain to the real one. */
#ifndef _UK_LINUX_FCNTL_H
#define _UK_LINUX_FCNTL_H
#if defined(__BIONIC__)
#include_next <linux/fcntl.h>
#else
#include <fcntl.h>
#ifndef O_RDONLY
#define O_RDONLY 0
#endif
#endif
#endif
