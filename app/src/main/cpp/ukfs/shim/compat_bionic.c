/* uKernel — bionic-compat shims for the Android FS-engine build.
 *
 * Symbols that glibc / the uKernel net-usb shim provide, but which the
 * FS-only (vfat/exfat/ntfs3/ext4) link on bionic lacks. All weak, so the real
 * definitions (fileio.c get_random_u32, ntfs3_stubs hex_asc, net_compat
 * system_wq, ...) win whenever those translation units are part of the build. */

#include <stddef.h>

/* --- glibc <execinfo.h> backtraces: not implemented in bionic libc at API 24.
 *     The slab heap-guard debug backtraces degrade to no-ops. Signatures match
 *     <execinfo.h> exactly so the declaration in slab.c stays consistent. --- */
__attribute__((weak)) int backtrace(void **buffer, int size)
{
	(void) buffer; (void) size;
	return 0;
}
__attribute__((weak)) void backtrace_symbols_fd(void *const *buffer, int size, int fd)
{
	(void) buffer; (void) size; (void) fd;
}

/* --- kernel lib hex helpers (lib/hexdump.c). hex_asc[] is also defined by
 *     ntfs3_stubs.c; weak here so that strong def wins in the ntfs3 build. --- */
__attribute__((weak)) const char hex_asc[]       = "0123456789abcdef";
__attribute__((weak)) const char hex_asc_upper[] = "0123456789ABCDEF";

static int uk_hex_to_bin(char ch)
{
	if (ch >= '0' && ch <= '9') return ch - '0';
	ch &= ~0x20;  /* upper */
	if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
	return -1;
}
__attribute__((weak)) int hex2bin(unsigned char *dst, const char *src, size_t count)
{
	while (count--) {
		int hi = uk_hex_to_bin(*src++);
		int lo = uk_hex_to_bin(*src++);
		if (hi < 0 || lo < 0) return -1;
		*dst++ = (unsigned char)((hi << 4) | lo);
	}
	return 0;
}

/* --- system workqueue global (defined by the net shim's net_compat.c, absent
 *     in the FS-only build). sched.c lazily alloc_workqueue()s it on first use. --- */
struct workqueue_struct;
__attribute__((weak)) struct workqueue_struct *system_wq;

/* --- get_random_u32 (fileio.c provides the real one; excluded from FS build).
 *     A small self-seeding xorshift is plenty for FAT volume serials etc. --- */
__attribute__((weak)) unsigned int get_random_u32(void)
{
	static unsigned int s = 0x9e3779b9u;
	s ^= s << 13; s ^= s >> 17; s ^= s << 5;
	return s;
}
