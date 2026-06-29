/* uKernel Wi-Fi — netlink engine glue: the `uknl_debug` flag the vendored
 * dispatch/genl_ctrl reference, and the daemon entry points (register + dispatch
 * a raw netlink request into a reply buffer). */
#include "ukwifi_nl.h"
#include "nl_dispatch.h"
#include "netlink_msg.h"
#include "sysfs.h"
#include <stdlib.h>
#include <string.h>

int uknl_debug = 0;   /* UK_NL_DEBUG -> 1 */

/* ---- uknl_common glue for the in-daemon (sync request/reply) model ----
 * In the LD_PRELOAD bridge these flushed/streamed over a socketpair; here a
 * handler builds its whole reply into `resp` and nl_dispatch returns it as one
 * UK_OP_NL response, so the early-flush helper is a no-op (content stays in
 * resp). Async multicast events (NEW_SCAN_RESULTS, MLME, …) are W3b-2. */
void uknl_send(struct nl_buf *b) { (void) b; }
void uknl_mcast_send(int group, struct nl_buf *b) { (void) group; (void) b; }
volatile unsigned uknl_scan_gen = 0;
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

int ukw_nl_dispatch(const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
	struct nl_buf b; nlb_init(&b);
	nl_dispatch(in, len, &b);
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
