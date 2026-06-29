/* uKernel Wi-Fi — netlink engine entry points for the daemon (UK_OP_NL). */
#ifndef UKWIFI_NL_H
#define UKWIFI_NL_H
#include <stdint.h>
#include <stddef.h>

/* Register the genl CTRL + nl80211 families (call once at daemon init). */
void ukw_nl_register(void);

/* Run one raw netlink request through the engine; write the reply into out.
 * Returns the reply length (bytes), or 0 if none. */
int  ukw_nl_dispatch(const uint8_t *in, size_t len, uint8_t *out, size_t cap);

/* Same, for an rtnetlink (NETLINK_ROUTE) request (ip link / ip addr). */
int  ukw_rtnl(const uint8_t *in, size_t len, uint8_t *out, size_t cap);

/* Async scan-event delivery (generation-based). ukw_nl_scangen() returns the
 * current scan generation (side-effect-free, for poll). ukw_nl_event(last_gen,
 * out, cap): if a newer scan completed, write a NEW_SCAN_RESULTS event into out
 * and set *cur_gen to the new generation; returns the event length (0 = none). */
unsigned ukw_nl_scangen(void);
int      ukw_nl_event(unsigned last_gen, unsigned *cur_gen, uint8_t *out, size_t cap);

#endif
