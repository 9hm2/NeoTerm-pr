/* uKernel <linux/socket.h>. On bionic, libc <sys/socket.h> pulls the real
 * kernel-uapi <linux/socket.h> (struct sockaddr/sockaddr_storage/sa_family_t);
 * the stub below would redefine them. Chain to the real header on bionic, keep
 * the stub on glibc (which never pulls kernel uapi from libc). */
#ifndef _UK_LINUX_SOCKET_H
#define _UK_LINUX_SOCKET_H
#if defined(__BIONIC__)
#include_next <linux/socket.h>
#else
#include <linux/types.h>
typedef unsigned short __kernel_sa_family_t;
typedef unsigned short sa_family_t;
struct sockaddr { unsigned short sa_family; char sa_data[14]; };
struct sockaddr_storage { unsigned short ss_family; char __data[126]; };
#endif
#endif
