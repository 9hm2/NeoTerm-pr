/* uKernel — file/ioctl proxy wire-protokoll (UNIX domain socket).
 *
 * Egy kapcsolat egy megnyitott eszközhöz tartozik: az első üzenet OPEN (a
 * payload az eszköz neve), utána READ/WRITE/IOCTL/POLL a megnyitott file-on. */
#ifndef UKERNEL_PROXY_H
#define UKERNEL_PROXY_H

#include <stdint.h>

#define UK_PROXY_SOCK_DEFAULT "/tmp/ukernel.sock"

enum uk_proxy_op {
	UK_OP_OPEN  = 1,   /* payload = eszköznév; cmd = open flags */
	UK_OP_CLOSE = 2,
	UK_OP_READ  = 3,   /* cmd = kért bájtszám; válasz payload = adat */
	UK_OP_WRITE = 4,   /* payload = írandó adat */
	UK_OP_IOCTL = 5,   /* cmd = ioctl parancs; payload = arg puffer (_IOC_SIZE) */
	UK_OP_POLL  = 6,   /* válasz ret = poll maszk */
	/* vezeték nélküli (nl80211 bridge foundation) — nem igényel megnyitott file-t */
	UK_OP_SCAN    = 10, /* cmd = wiphy idx; válasz ret = talált BSS-ek száma */
	UK_OP_GET_BSS = 11, /* cmd = (wiphy_idx<<16)|bss_idx; válasz payload = uk_bss_info */
	UK_OP_LIST_WIPHY = 12, /* válasz ret = wiphy-k száma */
	UK_OP_GET_IFACE  = 13, /* cmd = netdev idx; válasz payload = uk_iface_info */
	UK_OP_CONNECT    = 14, /* cmd = wiphy idx; payload = uk_connect_req; ret = státusz, payload = bssid[6] */
	UK_OP_EAPOL_TX   = 15, /* cmd = netdev idx; payload = ethernet-keret (EAPOL) -> ndo_start_xmit */
	UK_OP_EAPOL_RX   = 16, /* cmd = netdev idx; válasz payload = a következő RX EAPOL-keret (0=nincs) */
	UK_OP_DATA_TX    = 17, /* cmd = netdev idx; payload = TELJES ethernet-keret (IP/DHCP) */
	UK_OP_DATA_RX    = 18, /* cmd = netdev idx; válasz payload = a következő RX IP-csomag (0=nincs) */
	UK_OP_ADD_KEY    = 19, /* cmd = wiphy idx; payload = uk_key_req (PTK/GTK telepítés) */
	UK_OP_SET_MONITOR= 20, /* cmd = (wiphy_idx<<1)|enable; a chip VALÓDI monitor-módba */
	UK_OP_INJECT     = 21, /* cmd = netdev idx; payload = [radiotap][802.11] -> a levegőre */
	UK_OP_MONITOR_RX = 22, /* cmd = netdev idx; válasz payload = a következő monitor-keret */
	UK_OP_SET_CHANNEL= 23, /* cmd = (wiphy_idx<<16)|freq_MHz; a chip rögzítése a csatornára */
	UK_OP_SET_IFFLAGS= 24, /* cmd = (netdev_idx<<1)|up; ifconfig up/down -> ndo_open/ndo_stop */
	UK_OP_SET_IFADDR = 25, /* cmd = netdev idx; payload = [u32 ip][u32 netmask] (0 ip = törlés) */
	UK_OP_SET_MTU    = 26, /* cmd = (netdev_idx<<16)|mtu */
	UK_OP_SET_MAC    = 27, /* cmd = netdev idx; payload = 6 bájt MAC (macchanger/ip set address) */
	/* modul-kezelés: a guest `modprobe`/`rmmod`/`lsmod` parancsai a daemonon át
	 * a (előre fordított) vendor driver .so be-/kitöltését vezérlik. */
	UK_OP_MODPROBE   = 30, /* payload = modulnév (pl. "rtl8812au"); ret = 0/hiba */
	UK_OP_RMMOD      = 31, /* payload = modulnév; ret = 0/hiba */
	UK_OP_LSMOD      = 32, /* válasz payload = /proc/modules szöveg; ret = hossz */
	UK_OP_NL         = 33, /* payload = nyers netlink kérés; válasz payload = netlink válasz (genl/nl80211) */
	UK_OP_NL_EVENT   = 34, /* cmd = utoljára látott scan-gen; ret = aktuális gen, payload = NEW_SCAN_RESULTS ha új */
	UK_OP_NL_SCANGEN = 35, /* ret = aktuális scan-generáció (poll-próbához, mellékhatás nélkül) */
	UK_OP_RTNL       = 36, /* payload = nyers rtnetlink kérés (ip link/addr); válasz payload = rtnetlink válasz */
};

/* kulcs-telepítés a 4-way handshake után */
struct uk_key_req {
	int32_t  key_idx;
	int32_t  pairwise;
	uint8_t  mac[6];
	uint32_t cipher;
	int32_t  key_len;
	uint8_t  key[64];
	int32_t  seq_len;
	uint8_t  seq[16];
};

/* asszociációs kérés (WPA2-PSK) */
struct uk_connect_req {
	char     ssid[33];
	uint8_t  ssid_len;
	uint8_t  bssid[6];   /* 0 = a driver válasszon */
	int32_t  freq;       /* 0 = bármelyik */
	uint16_t ie_len;     /* a wpa_supplicant assoc-req IE-i (RSN-IE!) */
	uint8_t  ie[256];
};

/* egy hálózati interfész VALÓDI adatai (a driver netdev-jéből) */
struct uk_iface_info {
	char    name[16];   /* pl. "wlan0" */
	uint8_t mac[6];     /* a chip valódi MAC-je (efuse) */
	int32_t ifindex;
	int32_t wiphy_idx;
	uint32_t flags;     /* IFF_UP/RUNNING — a netdev/carrier VALÓS állapotából */
	uint32_t freq;      /* a chip AKTUÁLIS csatorna-frekvenciája (MHz) — 0=ismeretlen */
	/* VALÓDI hálózati állapot: DHCP-ből tanult cím + valós keret-számlálók */
	uint32_t ip;        /* IPv4 cím (host byte order); 0=nincs */
	uint32_t gw;        /* alapértelmezett átjáró */
	uint32_t netmask;   /* hálózati maszk */
	uint64_t tx_packets;
	uint64_t tx_bytes;
	uint64_t rx_packets;
	uint64_t rx_bytes;
	uint32_t mtu;       /* aktuális MTU (alap 1500, ifconfig/ip állíthatja) */
	uint8_t  gw_mac[6]; /* az átjáró MAC-je, ha a backend már látott tőle ARP-választ (0=nincs)
	                     * — a kliens-handlerek ezzel kihagyják a per-kapcsolat ARP-ot a flaky linken */
	uint8_t  _pad[2];
};

/* egy BSS (scan-eredmény) a kliensnek — egyezik a cfg80211.so belső formátumával */
struct uk_bss_info {
	char    ssid[33];
	uint8_t bssid[6];
	int32_t signal;   /* mBm */
	int32_t freq;     /* MHz */
	uint16_t cap;     /* beacon capability mező */
	uint16_t ie_len;  /* a nyers beacon-IE-k hossza */
	uint8_t  ie[256]; /* a VALÓDI beacon-IE-k (RSN/WPA -> helyes titkosítás) */
};

struct uk_req {
	uint32_t op;
	uint32_t cmd;
	uint32_t len;   /* az ezt követő payload hossza */
};

struct uk_rsp {
	int32_t  ret;   /* visszatérési érték (negatív = -errno) */
	uint32_t len;   /* az ezt követő payload hossza */
};

#endif
