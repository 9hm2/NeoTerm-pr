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

#endif
