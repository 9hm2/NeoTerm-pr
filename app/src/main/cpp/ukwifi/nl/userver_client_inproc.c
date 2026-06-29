/* uKernel Wi-Fi — in-process userver_client.
 *
 * The vendored nl80211 command handlers (nl80211_cmds.c) were written for the
 * LD_PRELOAD bridge, where uk_*() talked to the uServer over a UNIX socket
 * (UK_OP_*). Here they run INSIDE the daemon, so we implement the same
 * userver_client.h surface by calling the daemon's own functions directly
 * (resolved from our image via dlsym, exactly the symbols userver.c uses). No
 * socket round-trip. Only the entry points nl80211_cmds.c needs are provided;
 * the rtnetlink/packet ones come with W3b-2. */
#include "userver_client.h"
#include "ukernel/proxy.h"
#include <dlfcn.h>
#include <string.h>
#include <stdint.h>

const char *uk_sock_path(void) { return "(in-process)"; }

int uk_list_wiphy(void)
{
	static size_t (*f)(void);
	if (!f) f = dlsym(RTLD_DEFAULT, "ukernel_wiphy_count");
	return f ? (int) f() : -1;
}

int uk_scan(int wiphy_idx)
{
	static int (*f)(size_t);
	if (!f) f = dlsym(RTLD_DEFAULT, "ukernel_wiphy_scan");
	return f ? f((size_t) wiphy_idx) : -1;
}

int uk_get_bss(int wiphy_idx, int bss_idx, struct uk_bss_info *out)
{
	static int (*f)(size_t, size_t, struct uk_bss_info *);
	if (!f) f = dlsym(RTLD_DEFAULT, "ukernel_wiphy_bss_get");
	return (f && f((size_t) wiphy_idx, (size_t) bss_idx, out) == 0) ? 0 : -1;
}

/* Fill uk_iface_info from the live netdev (same fields userver's GET_IFACE uses). */
int uk_get_iface(int netdev_idx, struct uk_iface_info *out)
{
	static int (*nd_info)(int, char *, unsigned char *, unsigned int *);
	static int (*getfreq)(void);
	static uint32_t (*getmtu)(void);
	if (!nd_info)  nd_info  = dlsym(RTLD_DEFAULT, "ukernel_netdev_info");
	if (!getfreq)  getfreq  = dlsym(RTLD_DEFAULT, "ukernel_wiphy_get_chan_freq");
	if (!getmtu)   getmtu   = dlsym(RTLD_DEFAULT, "ukernel_netdev_get_mtu");
	memset(out, 0, sizeof *out);
	unsigned int flags = 0;
	if (!nd_info || nd_info(netdev_idx, out->name, out->mac, &flags) != 0) return -1;
	out->ifindex = netdev_idx + 3;
	out->wiphy_idx = 0;
	out->flags = flags;
	out->freq = getfreq ? (uint32_t) getfreq() : 0;
	out->mtu = getmtu ? getmtu() : 1500;
	return 0;
}

int uk_connect(int wiphy_idx, const struct uk_connect_req *req, uint8_t *bssid_out)
{
	/* signatures match userver.c's UK_OP_CONNECT handler exactly */
	static int (*f)(size_t, const char *, int, const uint8_t *, int, const uint8_t *, int);
	static int (*b)(uint8_t *);
	if (!f) f = dlsym(RTLD_DEFAULT, "ukernel_wiphy_connect");
	if (!b) b = dlsym(RTLD_DEFAULT, "ukernel_wiphy_conn_bssid");
	int r = f ? f((size_t) wiphy_idx, req->ssid, req->ssid_len, req->bssid, req->freq, req->ie, req->ie_len) : -1;
	if (bssid_out && b) b(bssid_out);
	return r;
}

int uk_add_key(int wiphy_idx, const struct uk_key_req *kr)
{
	/* signature matches userver.c's UK_OP_ADD_KEY handler */
	static int (*f)(size_t, int, int, const uint8_t *, uint32_t, const uint8_t *, int, const uint8_t *, int);
	if (!f) f = dlsym(RTLD_DEFAULT, "ukernel_wiphy_add_key");
	return f ? f((size_t) wiphy_idx, kr->key_idx, kr->pairwise, kr->mac, kr->cipher,
	            kr->key, kr->key_len, kr->seq, kr->seq_len) : -1;
}

int uk_set_channel(int wiphy_idx, int freq_mhz)
{
	static int (*f)(size_t, int);
	if (!f) f = dlsym(RTLD_DEFAULT, "ukernel_wiphy_set_channel");
	return f ? f((size_t) wiphy_idx, freq_mhz) : -1;
}

int uk_set_monitor(int wiphy_idx, int enable)
{
	static int (*f)(size_t, int);
	if (!f) f = dlsym(RTLD_DEFAULT, "ukernel_wiphy_set_monitor");
	return f ? f((size_t) wiphy_idx, enable) : -1;
}
