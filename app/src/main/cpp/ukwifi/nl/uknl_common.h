/* uKernel nl80211-bridge — közös konstansok. */
#ifndef UKNL_COMMON_H
#define UKNL_COMMON_H

/* A virtuális nl80211 család-id (a CTRL_CMD_GETFAMILY ezt adja vissza). */
#define UKNL_NL80211_FAMILY_ID  0x24

/* multicast csoport-id-k (a CTRL_CMD_GETFAMILY listázza) */
#define UKNL_MCGRP_CONFIG   1
#define UKNL_MCGRP_SCAN     2
#define UKNL_MCGRP_REG      3
#define UKNL_MCGRP_MLME     4
#define UKNL_MCGRP_VENDOR   5

/* a mi egyetlen wlan interfészünk (a uServer egy wiphy-t/netdev-et regisztrál) */
#define UKNL_IFINDEX   3
#define UKNL_WIPHY_IDX 0

extern int uknl_debug;  /* UK_NL_DEBUG env -> 1 */

/* Korai flush: egy handler a felépített üzeneteket AZONNAL kiküldheti (és üríti
 * a puffert) — pl. a TRIGGER_SCAN az ACK-ot a hosszú scan ELŐTT. A preload-réteg
 * implementálja, a handler hívja. */
struct nl_buf;
void uknl_send(struct nl_buf *b);

/* Egy mcast-esemény (pl. NEW_SCAN_RESULTS) kézbesítése MINDEN olyan netlink-
 * socketnek, ami feliratkozott a megadott csoportra (a wpa_supplicant/iw külön
 * event-socketen figyel). A handlerek hívják. */
void uknl_mcast_send(int group, struct nl_buf *b);

/* Scan-eredmény generáció: a TRIGGER_SCAN növeli, amikor friss BSS-cache áll elő.
 * A setsockopt(NETLINK_ADD_MEMBERSHIP) elkapó ezzel pótolja a NEW_SCAN_RESULTS-t
 * annak a socketnek, ami a trigger UTÁN iratkozott fel (race-mentes scan az iw-vel). */
extern volatile unsigned uknl_scan_gen;
void uknl_build_scan_event(struct nl_buf *b);

/* A vett ethernet-keret beírása a libpcap TPACKET_V2 RX-ringjébe (a packet-handler
 * hívja a hnd_fd alapján). 1 = ringbe írva, 0 = nincs ring (essen socketpair-re). */
#include <stdint.h>
int uknl_pkt_ring_write(int hnd_fd, const uint8_t *eth, int len);

#endif
