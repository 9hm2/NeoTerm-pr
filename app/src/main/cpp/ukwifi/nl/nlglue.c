/* uKernel Wi-Fi — netlink engine glue: the `uknl_debug` flag the vendored
 * dispatch/genl_ctrl reference, and the daemon entry points (register + dispatch
 * a raw netlink request into a reply buffer). */
#include "ukwifi_nl.h"
#include "nl_dispatch.h"
#include "netlink_msg.h"
#include "sysfs.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int uknl_debug = 0;   /* UK_NL_DEBUG -> 1 */

/* ---- uknl_common glue for the in-daemon (sync request/reply) model ----
 * In the LD_PRELOAD bridge these flushed/streamed over a socketpair; here a
 * handler builds its whole reply into `resp` and nl_dispatch returns it as one
 * UK_OP_NL response, so the early-flush helper is a no-op (content stays in
 * resp). Async multicast events (NEW_SCAN_RESULTS, MLME, …) are W3b-2. */
void uknl_send(struct nl_buf *b) { (void) b; }
volatile unsigned uknl_scan_gen = 0;

/* Generic async-event FIFO: nl80211 handlers (e.g. cmd_connect's NL80211_CMD_CONNECT
 * on the MLME group) emit events via uknl_mcast_send; UK_OP_NL_EVENT drains them to
 * the subscribed guest socket. (Scan results stay generation-based, see below.) */
#define UKW_EVQ 32
static struct { uint8_t *data; int len; } g_evq[UKW_EVQ];
static int g_evhead, g_evtail;   /* simple ring; head==tail => empty */
void uknl_mcast_send(int group, struct nl_buf *b)
{
	(void) group;
	if (!b || b->len == 0) return;
	int nxt = (g_evtail + 1) % UKW_EVQ;
	if (nxt == g_evhead) return;   /* full: drop (oldest kept) */
	uint8_t *c = (uint8_t *) malloc(b->len); if (!c) return;
	memcpy(c, b->data, b->len);
	g_evq[g_evtail].data = c; g_evq[g_evtail].len = (int) b->len;
	g_evtail = nxt;
}
static int evq_pop(uint8_t *out, int cap)
{
	if (g_evhead == g_evtail) return 0;
	int n = g_evq[g_evhead].len; if (n > cap) n = cap;
	if (out) memcpy(out, g_evq[g_evhead].data, n);
	free(g_evq[g_evhead].data); g_evq[g_evhead].data = NULL;
	g_evhead = (g_evhead + 1) % UKW_EVQ;
	return n;
}
void uknl_set_monitor_mode(int m) { (void) m; }   /* AF_PACKET monitor path = W3b-2 */

/* Per-vif sysfs hooks: the daemon's wsysfs writer already publishes
 * /sys/class/net + /sys/class/ieee80211 at probe, so these are no-ops here. */
void uknl_sysfs_add_iface(const char *name, int type) { (void) name; (void) type; }
void uknl_sysfs_del_iface(const char *name) { (void) name; }
void uknl_sysfs_set_type(const char *name, int type) { (void) name; (void) type; }

void ukw_nl_register(void)
{
	const char *d = getenv("UK_NL_DEBUG");
	uknl_debug = (d && *d && *d != '0') ? 1 : 0;
	genl_ctrl_register();
	nl80211_register();
}

static void ukw_hexdump(const char *tag, const uint8_t *p, size_t n)
{
	if (!uknl_debug) return;
	fprintf(stderr, "[uknl] %s (%zu b):", tag, n);
	for (size_t i = 0; i < n && i < 128; i++) fprintf(stderr, " %02x", p[i]);
	fprintf(stderr, "%s\n", n > 128 ? " ..." : "");
}
int ukw_nl_dispatch(const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
	struct nl_buf b; nlb_init(&b);
	ukw_hexdump("NL req", in, len);
	nl_dispatch(in, len, &b);
	size_t n = b.len < cap ? b.len : cap;
	if (n && out) memcpy(out, b.data, n);
	ukw_hexdump("NL resp", b.data, b.len);
	nlb_free(&b);
	return (int) n;
}

/* rtnetlink dispatch (ip link / ip addr) — defined in rtnetlink.c. */
extern void ukw_rtnl_dispatch(const uint8_t *buf, size_t n, struct nl_buf *resp);
int ukw_rtnl(const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
	struct nl_buf b; nlb_init(&b);
	ukw_rtnl_dispatch(in, len, &b);
	size_t n = b.len < cap ? b.len : cap;
	if (n && out) memcpy(out, b.data, n);
	nlb_free(&b);
	return (int) n;
}

/* uknl_build_scan_event() is defined by nl80211_cmds.c; uknl_scan_gen above. */
extern void uknl_build_scan_event(struct nl_buf *ev);

unsigned ukw_nl_scangen(void) { return uknl_scan_gen; }

int ukw_nl_event(unsigned last_gen, unsigned *cur_gen, uint8_t *out, size_t cap)
{
	/* generic queued events (connect/MLME/...) first — gen unchanged so the
	 * client keeps polling for more before advancing the scan generation. */
	int q = evq_pop(out, (int) cap);
	if (q > 0) { if (cur_gen) *cur_gen = last_gen; return q; }

	unsigned g = uknl_scan_gen;
	if (cur_gen) *cur_gen = g;
	if (g == last_gen) return 0;            /* no new scan results */
	struct nl_buf b; nlb_init(&b);
	uknl_build_scan_event(&b);
	size_t n = b.len < cap ? b.len : cap;
	if (n && out) memcpy(out, b.data, n);
	nlb_free(&b);
	return (int) n;
}
