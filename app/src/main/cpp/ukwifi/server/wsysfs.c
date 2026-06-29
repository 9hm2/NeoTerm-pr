/* uKernel Wi-Fi — fake /sys/class/net + /sys/class/ieee80211 writer (see wsysfs.h). */
#include "wsysfs.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/stat.h>

static void wfile(const char *dir, const char *name, const char *fmt, ...)
{
	char path[1024]; snprintf(path, sizeof path, "%s/%s", dir, name);
	FILE *f = fopen(path, "w"); if (!f) return;
	va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fclose(f);
}
static void wmac(const char *dir, const char *name, const unsigned char *m)
{
	wfile(dir, name, "%02x:%02x:%02x:%02x:%02x:%02x\n", m[0], m[1], m[2], m[3], m[4], m[5]);
}

void ukw_wsysfs_refresh(void)
{
	const char *netdir = getenv("UK_WIFI_SYSFS_NET");
	const char *phydir = getenv("UK_WIFI_SYSFS_PHY");
	if (!netdir || !phydir) return;

	/* live state accessors (defined by the net shim / cfg80211 in our image) */
	int  (*nd_count)(void)                                      = dlsym(RTLD_DEFAULT, "ukernel_netdev_count");
	int  (*nd_info)(int, char *, unsigned char *, unsigned int *) = dlsym(RTLD_DEFAULT, "ukernel_netdev_info");
	unsigned int (*nd_mtu)(void)                                = dlsym(RTLD_DEFAULT, "ukernel_netdev_get_mtu");
	size_t (*wp_count)(void)                                    = dlsym(RTLD_DEFAULT, "ukernel_wiphy_count");
	const char *(*wp_name)(size_t)                              = dlsym(RTLD_DEFAULT, "ukernel_wiphy_name");
	if (!nd_count || !nd_info) return;

	int nn = nd_count();
	for (int i = 0; i < nn; i++) {
		char name[16]; unsigned char mac[6] = {0}; unsigned int flags = 0;
		memset(name, 0, sizeof name);
		if (nd_info(i, name, mac, &flags) != 0 || !name[0]) continue;
		char d[1024]; snprintf(d, sizeof d, "%s/%s", netdir, name);
		mkdir(d, 0755);
		wmac(d, "address", mac);
		wfile(d, "addr_len", "6\n");
		wfile(d, "ifindex", "%d\n", i + 3);          /* netdev[0] -> ifindex 3 (matches UK_OP_GET_IFACE) */
		wfile(d, "type", "1\n");                      /* ARPHRD_ETHER */
		wfile(d, "flags", "0x%x\n", flags ? flags : 0x1003);  /* UP|BROADCAST|MULTICAST default */
		wfile(d, "mtu", "%u\n", nd_mtu ? nd_mtu() : 1500u);
		wfile(d, "operstate", (flags & 1 /*IFF_UP*/) ? "up\n" : "down\n");
		wfile(d, "carrier", (flags & 1) ? "1\n" : "0\n");
		wfile(d, "uevent", "INTERFACE=%s\nIFINDEX=%d\n", name, i + 3);
		/* phy80211 symlink -> /sys/class/ieee80211/phy<wiphy> (assume phy0 for now) */
		char link[1100]; snprintf(link, sizeof link, "%s/phy80211", d);
		unlink(link);
		if (symlink("../../ieee80211/phy0", link) != 0) { /* best-effort */ }
	}

	if (wp_count && wp_name) {
		size_t nw = wp_count();
		for (size_t i = 0; i < nw; i++) {
			const char *pn = wp_name(i); if (!pn || !*pn) continue;
			char d[1024]; snprintf(d, sizeof d, "%s/%s", phydir, pn);
			mkdir(d, 0755);
			wfile(d, "name", "%s\n", pn);
			wfile(d, "index", "%zu\n", i);
			wfile(d, "address_mask", "00:00:00:00:00:00\n");
			/* macaddress: reuse the first netdev's MAC (single-chip case) */
			if (nn > 0) {
				char nm[16]; unsigned char mac[6] = {0}; unsigned int fl = 0;
				memset(nm, 0, sizeof nm);
				if (nd_info(0, nm, mac, &fl) == 0) wmac(d, "macaddress", mac);
			}
		}
	}
}
