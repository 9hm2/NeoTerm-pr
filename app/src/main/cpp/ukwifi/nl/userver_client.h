/* uKernel nl80211-bridge — uServer proxy-kliens.
 * A valós wiphy/scan/BSS adatot a uServer UNIX-socketjéből (proxy) kéri le. */
#ifndef UKNL_USERVER_CLIENT_H
#define UKNL_USERVER_CLIENT_H

#include <stdint.h>
#include "ukernel/proxy.h"   /* struct uk_bss_info, op-kódok */

/* A socket útvonala (UK_SOCK env felülírja, alap: UK_PROXY_SOCK_DEFAULT). */
const char *uk_sock_path(void);

int    uk_list_wiphy(void);                 /* wiphy-k száma, <0 hiba */
int    uk_scan(int wiphy_idx);              /* scan -> talált BSS-ek száma */
int    uk_get_bss(int wiphy_idx, int bss_idx, struct uk_bss_info *out);  /* 0 = ok */
int    uk_get_iface(int netdev_idx, struct uk_iface_info *out);          /* 0 = ok */
int    uk_connect(int wiphy_idx, const struct uk_connect_req *req, uint8_t *bssid_out);  /* status */
int    uk_eapol_tx(int netdev_idx, const uint8_t *frame, int len);   /* EAPOL wpa -> chip */
int    uk_eapol_rx(int netdev_idx, uint8_t *out, int cap);           /* EAPOL chip -> wpa (0=nincs) */
int    uk_data_tx(int netdev_idx, const uint8_t *frame, int len);    /* IP/DHCP wpa -> chip */
int    uk_data_rx(int netdev_idx, uint8_t *out, int cap);            /* IP/DHCP chip -> wpa */
int    uk_add_key(int wiphy_idx, const struct uk_key_req *kr);       /* PTK/GTK telepítés */
int    uk_set_monitor(int wiphy_idx, int enable);                    /* chip VALÓDI monitor-módba */
int    uk_inject(int netdev_idx, const uint8_t *frame, int len);     /* [radiotap][802.11] -> levegő */
int    uk_monitor_rx(int netdev_idx, uint8_t *out, int cap);         /* levegőből vett keret */
int    uk_set_channel(int wiphy_idx, int freq_mhz);                  /* chip rögzítése csatornára */
int    uk_set_ifflags(int netdev_idx, int up);                       /* ifconfig up/down -> ndo_open/stop */
int    uk_set_ifaddr(int netdev_idx, uint32_t ip, uint32_t netmask);  /* kézi IP/maszk (0 ip = törlés) */
int    uk_set_mtu(int netdev_idx, int mtu);                          /* MTU beállítása */
int    uk_set_mac(int netdev_idx, const uint8_t *mac);               /* MAC beállítása (macchanger) */

/* Cache-elt VALÓS interfész-info: ha kapcsolódva van, de még nincs IP, lefuttat
 * egy DHCP-próbát (a szerver tanulja a címet), majd újra lekérdez. 0 = ok. */
struct uk_iface_info;
int    uknl_iface_info(struct uk_iface_info *out);

#endif
