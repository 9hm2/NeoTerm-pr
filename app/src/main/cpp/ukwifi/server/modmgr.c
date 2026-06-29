/* uKernel Wi-Fi — module manager (see modmgr.h). */
#include "modmgr.h"
#include "wsysfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/stat.h>

#define UKW_MAXMOD 16
struct mod {
	int    used;
	char   name[64];
	void  *handle;
	size_t size;     /* bytes — shown by lsmod */
};
static struct mod g_mods[UKW_MAXMOD];
static char g_moddir[512];
static struct ukw_mod_ops g_ops;
static uint16_t g_vid, g_pid;

void ukw_modmgr_init(const char *moddir, const struct ukw_mod_ops *ops,
                     uint16_t vid, uint16_t pid)
{
	if (ops) g_ops = *ops;
	g_vid = vid; g_pid = pid;
	const char *d = moddir;
	if (!d || !*d) d = getenv("UK_WIFI_MODDIR");
	snprintf(g_moddir, sizeof g_moddir, "%s", d && *d ? d : ".");
}

static struct mod *find(const char *name)
{
	for (int i = 0; i < UKW_MAXMOD; i++)
		if (g_mods[i].used && strcmp(g_mods[i].name, name) == 0) return &g_mods[i];
	return NULL;
}
static struct mod *slot(void)
{
	for (int i = 0; i < UKW_MAXMOD; i++) if (!g_mods[i].used) return &g_mods[i];
	return NULL;
}
static size_t file_size(const char *path)
{
	struct stat st; return stat(path, &st) == 0 ? (size_t)st.st_size : 0;
}

void ukw_modmgr_note(const char *name, void *handle, size_t size)
{
	if (find(name)) return;
	struct mod *m = slot(); if (!m) return;
	m->used = 1; m->handle = handle; m->size = size;
	snprintf(m->name, sizeof m->name, "%s", name);
}

/* Resolve a module name to a vendor driver .so path: <moddir>/<name>.so or
 * <moddir>/lib<name>.so. Returns 1 + fills path, or 0 if none exists. */
static int resolve(const char *name, char *path, size_t cap)
{
	snprintf(path, cap, "%s/%s.so", g_moddir, name);
	if (file_size(path)) return 1;
	snprintf(path, cap, "%s/lib%s.so", g_moddir, name);
	if (file_size(path)) return 1;
	return 0;
}

int ukw_modprobe(const char *name)
{
	if (!name || !*name) return -1;
	if (find(name)) return 0;                 /* already loaded — like modprobe no-op */
	char path[640];
	if (!resolve(name, path, sizeof path)) {
		fprintf(stderr, "ukwifi/modmgr: nincs modul .so: %s (moddir=%s)\n", name, g_moddir);
		return -2;                            /* modprobe: module not found */
	}
	struct mod *m = slot();
	if (!m) return -3;
	void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);   /* driver resolves cfg80211/usb/shim from our image */
	if (!h) { fprintf(stderr, "ukwifi/modmgr: dlopen %s: %s\n", path, dlerror()); return -4; }
	if (g_ops.run_inits && g_ops.run_inits() != 0)
		fprintf(stderr, "ukwifi/modmgr: %s module_init hiba (folytatjuk)\n", name);
	if (g_ops.probe) {
		int n = g_ops.probe(g_vid, g_pid);
		fprintf(stderr, "ukwifi/modmgr: %s probe -> %d eszköz\n", name, n);
	}
	ukw_wsysfs_refresh();   /* publish /sys/class/net + /sys/class/ieee80211 */
	m->used = 1; m->handle = h; m->size = file_size(path);
	snprintf(m->name, sizeof m->name, "%s", name);
	return 0;
}

int ukw_rmmod(const char *name)
{
	struct mod *m = find(name);
	if (!m) return -1;
	if (g_ops.run_exits) g_ops.run_exits();
	if (m->handle) dlclose(m->handle);
	memset(m, 0, sizeof *m);
	return 0;
}

int ukw_lsmod(char *buf, size_t cap)
{
	/* /proc/modules format: name size refcount deps state addr */
	size_t off = 0;
	for (int i = 0; i < UKW_MAXMOD && off < cap; i++) {
		if (!g_mods[i].used) continue;
		int w = snprintf(buf + off, cap - off, "%s %zu 0 - Live 0x0000000000000000\n",
		                 g_mods[i].name, g_mods[i].size ? g_mods[i].size : 16384);
		if (w < 0) break;
		off += (size_t)w;
	}
	return (int)off;
}
