/* uKernel Wi-Fi — module manager: makes the guest's `modprobe` / `lsmod` /
 * `rmmod` drive the daemon. A name (e.g. "rtl8812au") resolves to a pre-built
 * vendor driver .so under the module dir; loading it = dlopen + run the kernel
 * module_init(s) + probe the device. `lsmod` reads the registry as a
 * /proc/modules-style listing. The driver is still a .so under the hood — only
 * the user-facing commands are the standard kernel-module ones. */
#ifndef UKWIFI_MODMGR_H
#define UKWIFI_MODMGR_H
#include <stddef.h>
#include <stdint.h>

/* Callbacks into the shim (resolved by userver via dlsym), so modmgr stays
 * decoupled from how the symbols are obtained. */
struct ukw_mod_ops {
	int  (*run_inits)(void);            /* run pending module_init()s */
	void (*run_exits)(void);            /* run module_exit()s */
	int  (*probe)(uint16_t, uint16_t);  /* enumerate_and_probe(vid,pid); 0/any = all granted */
};

/* moddir: where <name>.so / lib<name>.so live (NULL -> $UK_WIFI_MODDIR or the
 * daemon's own dir). vid/pid: the chip to probe on load (0 = any granted). */
void ukw_modmgr_init(const char *moddir, const struct ukw_mod_ops *ops,
                     uint16_t vid, uint16_t pid);

int  ukw_modprobe(const char *name);   /* 0 ok, <0 -errno-ish */
int  ukw_rmmod(const char *name);      /* 0 ok, <0 if not loaded */
int  ukw_lsmod(char *buf, size_t cap); /* /proc/modules text; returns length */
/* Record an already-dlopen'd module (e.g. one given on argv) so lsmod sees it. */
void ukw_modmgr_note(const char *name, void *handle, size_t size);

#endif
