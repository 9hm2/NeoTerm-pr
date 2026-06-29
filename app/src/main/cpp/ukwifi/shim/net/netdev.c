/* uKernel — net_device implementáció. */
#include <linux/netdevice.h>
#include <net/cfg80211.h>   /* struct wireless_dev (ieee80211_ptr) */
#include <linux/kernel.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dlfcn.h>   /* a driver rtw_dev_unload-jának eléréséhez (RTLD_GLOBAL) */

struct net_device *alloc_netdev_mqs(int sizeof_priv, const char *name, unsigned char assign,
                                    void (*setup)(struct net_device *), unsigned int txqs, unsigned int rxqs)
{
	(void)assign; (void)txqs; (void)rxqs;
	struct net_device *dev = calloc(1, sizeof(*dev));
	if (!dev) return NULL;
	if (sizeof_priv > 0) dev->priv = calloc(1, sizeof_priv);
	snprintf(dev->name, IFNAMSIZ, "%s", name ? name : "wlan%d");
	if (setup) setup(dev);
	return dev;
}
void free_netdev(struct net_device *dev) { if (dev) { free(dev->priv); free(dev); } }

/* regisztrált netdev-ek (a cfg80211/uServer innen találja meg a wlan0-t + wdev-et) */
#define MAX_NETDEV 8
static struct net_device *g_netdev[MAX_NETDEV];
static int g_nnetdev;

static int g_ifindex;
int register_netdev(struct net_device *dev)
{
	/* a "%d" névsablont feloldjuk */
	if (strchr(dev->name, '%')) { char b[IFNAMSIZ]; snprintf(b, sizeof(b), dev->name, g_ifindex++); snprintf(dev->name, IFNAMSIZ, "%s", b); }
	if (g_nnetdev < MAX_NETDEV) g_netdev[g_nnetdev++] = dev;
	printk(KERN_INFO "uKernel/net: register_netdev('%s')\n", dev->name);
	return 0;
}
void unregister_netdev(struct net_device *dev)
{
	printk(KERN_INFO "uKernel/net: unregister_netdev('%s')\n", dev->name);
	for (int i = 0; i < g_nnetdev; i++) if (g_netdev[i] == dev) g_netdev[i] = NULL;
}

/* === uServer/cfg80211 hozzáférés a regisztrált netdev-ekhez === */
int ukernel_netdev_count(void) { return g_nnetdev; }
struct net_device *ukernel_netdev_get(int i) { return (i >= 0 && i < g_nnetdev) ? g_netdev[i] : NULL; }

/* A VALÓDI interfész-adatok kitöltése a net_device-ból (a proxy/bridge ezt adja
 * az iw-nek: valódi név + a chip efuse-MAC-je). 0 = ok. */
int ukernel_netdev_carrier(void);
int ukernel_netdev_info(int i, char *name, unsigned char *mac, unsigned int *flags)
{
	struct net_device *d = ukernel_netdev_get(i);
	if (!d) return -1;
	if (name) { memcpy(name, d->name, IFNAMSIZ); name[IFNAMSIZ - 1] = 0; }
	if (mac)  memcpy(mac, d->dev_addr, 6);
	/* VALÓS flag-ek: IFF_UP az admin-állapotból, IFF_RUNNING a carrier-ből (asszociáció),
	 * + BROADCAST/MULTICAST (egy normál ethernet/wlan interfész). */
	if (flags) {
		unsigned int f = (d->flags & IFF_UP) | IFF_BROADCAST | IFF_MULTICAST;
		if (ukernel_netdev_carrier()) f |= IFF_RUNNING;
		*flags = f;
	}
	return 0;
}
/* carrier (link/asszociáció) — a cfg80211 connect/add_key állítja, a disconnect törli */
static int g_carrier;
void ukernel_netdev_set_carrier(int on) { g_carrier = on ? 1 : 0; }
int ukernel_netdev_carrier(void) { return g_carrier; }
/* admin up/down (ifconfig/ip up/down). A LED-et is kapcsoljuk:
 *  - down: ndo_stop (disconnect + a driver LED_CTL_POWER_OFF-ot ad -> LED off).
 *  - up:   ndo_open, majd rtw_led_control(LED_CTL_NO_LINK) -> LED vissza. (Az ndo_open
 *          a close után a bup/hw_init őrön no-op, ezért nem re-lightolna; ezért hívjuk
 *          a LED-et közvetlenül.) NEM hívunk rtw_dev_unload-ot: az a userspace-szál-
 *          emulációban beragadna (a driver-threadekre várna). A driver RTLD_GLOBAL,
 *          így a shim dlsym-eli a rtw_led_control-t; padapter a net_device priv-jéből. */
int ukernel_netdev_set_up(int idx, int up)
{
	struct net_device *dev = ukernel_netdev_get(idx);
	if (!dev || !dev->netdev_ops) return -1;
	void *padapter = dev->priv ? *(void **)dev->priv : NULL;
	static void (*led_ctl)(void *, int);
	if (!led_ctl) led_ctl = dlsym(RTLD_DEFAULT, "rtw_led_control");
	if (up) {
		if (!(dev->flags & IFF_UP)) {
			if (dev->netdev_ops->ndo_open) { int r = dev->netdev_ops->ndo_open(dev); if (r) return r; }
			dev->flags |= IFF_UP;
		}
		if (led_ctl && padapter) led_ctl(padapter, 3 /*LED_CTL_NO_LINK*/);
		return 0;
	}
	if (dev->flags & IFF_UP) {
		if (dev->netdev_ops->ndo_stop) dev->netdev_ops->ndo_stop(dev);
		dev->flags &= ~IFF_UP; g_carrier = 0;
	}
	return 0;
}
/* MAC-cím beállítása (macchanger / ip link set address): a driver ndo_set_mac_address-e
 * a chipre írja, és frissítjük a netdev dev_addr-t (a lekérdezések ezt adják vissza). */
int ukernel_netdev_set_mac(int idx, const unsigned char *mac)
{
	struct net_device *dev = ukernel_netdev_get(idx);
	if (!dev || !mac) return -1;
	if (dev->netdev_ops && dev->netdev_ops->ndo_set_mac_address) {
		/* layout-compatible with struct sockaddr (sa_family + sa_data[14]); the
		 * driver casts the void* arg back to sockaddr. Avoids depending on the
		 * libc struct sockaddr, which bionic's <linux/socket.h> does not define. */
		struct { unsigned short sa_family; char sa_data[14]; } sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_family = 1 /*ARPHRD_ETHER*/; memcpy(sa.sa_data, mac, 6);
		dev->netdev_ops->ndo_set_mac_address(dev, &sa);
	}
	memcpy(dev->dev_addr, mac, 6);   /* a lekérdezések (GET_IFACE/SIOCGIFHWADDR) ezt adják */
	return 0;
}
/* a megadott wiphy-hez tartozó netdev (ieee80211_ptr->wiphy egyezés) */
struct net_device *ukernel_netdev_for_wiphy(void *wiphy)
{
	for (int i = 0; i < g_nnetdev; i++) {
		struct net_device *d = g_netdev[i];
		if (d && d->ieee80211_ptr && ((struct wireless_dev *)d->ieee80211_ptr)->wiphy == wiphy)
			return d;
	}
	return NULL;
}
/* a netdev "up" hozása: ndo_open meghívása (teljes hardver-init + firmware) */
int ukernel_netdev_open(struct net_device *dev)
{
	if (!dev || !dev->netdev_ops || !dev->netdev_ops->ndo_open) return -1;
	printk(KERN_INFO "uKernel/net: ndo_open('%s')\n", dev->name);
	int r = dev->netdev_ops->ndo_open(dev);
	if (r == 0) dev->flags |= IFF_UP | IFF_RUNNING;
	return r;
}

/* ===== EAPOL (4-way handshake) adatút =====
 * A driver az AP-tól jövő adat-kereteket netif_rx-szel adja fel. Az EAPOL-okat
 * (ethertype 0x888E) egy sorba tesszük, hogy a wpa_supplicant (a bridge-en át)
 * kiolvashassa; a TX-et (wpa -> AP) az ndo_start_xmit-en át küldjük. */
#include <pthread.h>
#define EAPOL_Q 16
#define EAPOL_MAX 2048
static struct { uint8_t buf[EAPOL_MAX]; int len; } g_eapolq[EAPOL_Q];
static int g_eq_head, g_eq_tail;
static pthread_mutex_t g_eq_lock = PTHREAD_MUTEX_INITIALIZER;

static void eapol_enqueue(const uint8_t *f, int len)
{
	if (len <= 0 || len > EAPOL_MAX) return;
	pthread_mutex_lock(&g_eq_lock);
	int nh = (g_eq_head + 1) % EAPOL_Q;
	if (nh != g_eq_tail) {   /* van hely */
		memcpy(g_eapolq[g_eq_head].buf, f, len);
		g_eapolq[g_eq_head].len = len;
		g_eq_head = nh;
	}
	pthread_mutex_unlock(&g_eq_lock);
}
/* a bridge ezt pollozza: a következő EAPOL-keret (0 = nincs) */
int ukernel_eapol_rx_get(uint8_t *out, int cap)
{
	int n = 0;
	pthread_mutex_lock(&g_eq_lock);
	if (g_eq_tail != g_eq_head) {
		n = g_eapolq[g_eq_tail].len; if (n > cap) n = cap;
		memcpy(out, g_eapolq[g_eq_tail].buf, n);
		g_eq_tail = (g_eq_tail + 1) % EAPOL_Q;
	}
	pthread_mutex_unlock(&g_eq_lock);
	return n;
}

/* ===== általános adat-RX (DHCP/IP/ARP) — külön sor =====
 * NAGY sor + tele esetén a LEGRÉGEBBIT dobjuk: forgalmas hálón a broadcast IP-k ne
 * nyomják ki a fontos kereteket (pl. ARP-válasz, DHCP OFFER/ACK) -> stall ellen. */
#define DATA_Q 256
static struct { uint8_t buf[EAPOL_MAX]; int len; } g_dataq[DATA_Q];
static int g_dq_head, g_dq_tail;
static pthread_mutex_t g_dq_lock = PTHREAD_MUTEX_INITIALIZER;
/* a keretet [ethertype(2)][payload] formában tárolja, hogy a kliens megkülönböztesse
 * az ARP-ot (0x0806) és az IP-t (0x0800) */
static void data_enqueue(uint16_t ethertype, const uint8_t *f, int len)
{
	if (len <= 0 || len + 2 > EAPOL_MAX) return;
	pthread_mutex_lock(&g_dq_lock);
	int nh = (g_dq_head + 1) % DATA_Q;
	if (nh == g_dq_tail) g_dq_tail = (g_dq_tail + 1) % DATA_Q;   /* tele -> a legrégebbit eldobjuk */
	g_dataq[g_dq_head].buf[0] = ethertype >> 8; g_dataq[g_dq_head].buf[1] = ethertype;
	memcpy(g_dataq[g_dq_head].buf + 2, f, len); g_dataq[g_dq_head].len = len + 2; g_dq_head = nh;
	pthread_mutex_unlock(&g_dq_lock);
}
int ukernel_data_rx_get(uint8_t *out, int cap)
{
	int n = 0;
	pthread_mutex_lock(&g_dq_lock);
	if (g_dq_tail != g_dq_head) {
		n = g_dataq[g_dq_tail].len; if (n > cap) n = cap;
		memcpy(out, g_dataq[g_dq_tail].buf, n);
		g_dq_tail = (g_dq_tail + 1) % DATA_Q;
	}
	pthread_mutex_unlock(&g_dq_lock);
	return n;
}

/* ===== monitor-RX sor (VALÓDI levegőből vett radiotap+802.11 keretek) =====
 * NAGY sor (a beaconok elárasztják, a probe-response-ok ne essenek ki), és a
 * kiolvasás BATCH: egy hívással több keretet ad vissza [len(2 LE)][keret]... */
#define MON_Q   512
#define MON_MAX 3200   /* a max 802.11 + radiotap is elfér (a nagy keretek ne vesszenek) */
static struct { uint8_t buf[MON_MAX]; int len; } g_monq[MON_Q];
static int g_mq_head, g_mq_tail;
static pthread_mutex_t g_mq_lock = PTHREAD_MUTEX_INITIALIZER;
static void mon_enqueue(const uint8_t *f, int len)
{
	if (len <= 0 || len > MON_MAX) return;
	pthread_mutex_lock(&g_mq_lock);
	int nh = (g_mq_head + 1) % MON_Q;
	if (nh == g_mq_tail) g_mq_tail = (g_mq_tail + 1) % MON_Q;   /* tele -> a legrégebbit eldobjuk */
	memcpy(g_monq[g_mq_head].buf, f, len); g_monq[g_mq_head].len = len; g_mq_head = nh;
	pthread_mutex_unlock(&g_mq_lock);
}
/* batch: amennyi belefér, [len(2 LE)][keret]... formában */
int ukernel_monitor_rx_get(uint8_t *out, int cap)
{
	int total = 0;
	pthread_mutex_lock(&g_mq_lock);
	while (g_mq_tail != g_mq_head) {
		int n = g_monq[g_mq_tail].len;
		if (total + 2 + n > cap) break;
		out[total] = n & 0xff; out[total + 1] = (n >> 8) & 0xff;
		memcpy(out + total + 2, g_monq[g_mq_tail].buf, n);
		total += 2 + n;
		g_mq_tail = (g_mq_tail + 1) % MON_Q;
	}
	pthread_mutex_unlock(&g_mq_lock);
	return total;
}

/* ===== VALÓS interfész-állapot: keret-számlálók + DHCP-ből tanult cím ===== */
static volatile uint64_t g_tx_packets, g_tx_bytes, g_rx_packets, g_rx_bytes;
static uint32_t g_if_ip, g_if_gw, g_if_mask;        /* DHCP-ből tanulva (host byte order) */
static uint32_t g_admin_ip, g_admin_mask;           /* ifconfig/ip kézi beállítás (felülírja a DHCP-t) */
static uint32_t g_mtu = 1500;                       /* aktuális MTU */
static uint8_t  g_gw_mac[6]; static int g_have_gwmac; /* az átjáró MAC-je (ARP-válaszból tanulva) */
/* az átjáró MAC-jének lekérése (a kliens-handlerek ezzel kihagyják a per-kapcsolat ARP-ot) */
int ukernel_netdev_get_gwmac(uint8_t *out)
{ if (!g_have_gwmac) return -1; if (out) memcpy(out, g_gw_mac, 6); return 0; }
void ukernel_netdev_get_counters(uint64_t *txp, uint64_t *txb, uint64_t *rxp, uint64_t *rxb)
{ if (txp) *txp = g_tx_packets; if (txb) *txb = g_tx_bytes; if (rxp) *rxp = g_rx_packets; if (rxb) *rxb = g_rx_bytes; }
void ukernel_netdev_get_ipinfo(uint32_t *ip, uint32_t *gw, uint32_t *mask)
{
	/* kézi (admin) cím elsőbbsége a DHCP-vel szemben */
	if (ip)   *ip   = g_admin_ip ? g_admin_ip : g_if_ip;
	if (mask) *mask = g_admin_ip ? g_admin_mask : g_if_mask;
	if (gw)   *gw   = g_if_gw;
}
/* ifconfig/ip kézi cím-beállítás (ip=0 -> törlés, vissza a DHCP-re) */
void ukernel_netdev_set_addr(uint32_t ip, uint32_t mask)
{ g_admin_ip = ip; g_admin_mask = mask ? mask : 0xffffff00; if (!ip) g_admin_mask = 0; }
void ukernel_netdev_set_mtu(uint32_t mtu) { if (mtu >= 68 && mtu <= 65535) g_mtu = mtu; }
uint32_t ukernel_netdev_get_mtu(void) { return g_mtu; }

/* DHCP-sniff a VALÓDI forgalomból: az OFFER/ACK-ből kiolvassuk a saját IP-t (yiaddr),
 * az átjárót (opt 3) és a maszkot (opt 1). Az 'ip' az IP-csomag (eth-fejléc lehúzva). */
static void dhcp_sniff(const uint8_t *ip, int len)
{
	if (len < 240 + 8 || (ip[0] >> 4) != 4 || ip[9] != 17) return;
	int ihl = (ip[0] & 0xf) * 4;
	const uint8_t *udp = ip + ihl;
	if (((udp[2] << 8) | udp[3]) != 68) return;          /* dst port 68 = DHCP-kliens */
	const uint8_t *d = udp + 8;
	if (len < (ihl + 8 + 240)) return;
	uint32_t yi = (d[16] << 24) | (d[17] << 16) | (d[18] << 8) | d[19];
	const uint8_t *o = d + 240, *end = ip + len; int mtype = 0;
	uint32_t gw = 0, mask = 0;
	while (o + 2 <= end && *o != 255) {
		if (*o == 0) { o++; continue; }
		int t = o[0], l = o[1]; if (o + 2 + l > end) break;
		if (t == 53 && l == 1) mtype = o[2];
		if (t == 1  && l == 4) mask = (o[2]<<24)|(o[3]<<16)|(o[4]<<8)|o[5];
		if (t == 3  && l >= 4) gw   = (o[2]<<24)|(o[3]<<16)|(o[4]<<8)|o[5];
		o += 2 + l;
	}
	if ((mtype == 2 || mtype == 5) && yi) {   /* OFFER vagy ACK */
		g_if_ip = yi; if (gw) g_if_gw = gw; if (mask) g_if_mask = mask;
		if (!g_if_mask) g_if_mask = 0xffffff00;   /* /24 default */
	}
}

/* a driver RX-feladása: az EAPOL-kereteket elkapjuk, a többit eldobjuk */
static int g_rx_count;
static int rx_capture(struct sk_buff *skb)
{
	g_rx_count++;
	if (skb && skb->data && skb->len > 0 && skb->protocol != 0x1900) {
		g_rx_packets++; g_rx_bytes += skb->len;
	}
	/* monitor-keret (rtw_recv_monitor): radiotap+802.11, protocol ETH_P_80211_RAW */
	if (skb && skb->data && skb->len >= 8 && skb->protocol == 0x1900) {
		mon_enqueue(skb->data, skb->len);
		printk(KERN_INFO "uKernel/net: MONITOR RX (%u bájt) a levegőből\n", skb->len);
		kfree_skb(skb);
		return 0;
	}
	if (skb && skb->data && skb->len >= 14) {
		const uint8_t *d = skb->data;
		/* EAPOL-Key: az eth_type_trans lehúzta a fejlécet, a keret a payload
		 * (version 1-3, type 0x03 = EAPOL-Key). A wpa SOCK_DGRAM-mal a payload-ot várja. */
		int is_eapol = (d[12] == 0x88 && d[13] == 0x8e) ||
		               (d[0] >= 1 && d[0] <= 3 && d[1] == 0x03 && skb->len >= 4);
		if (is_eapol) {
			/* ha ethernet-fejléccel jött, a payload offset 14-től */
			const uint8_t *pl = (d[12] == 0x88 && d[13] == 0x8e) ? d + 14 : d;
			int pl_len = (d[12] == 0x88 && d[13] == 0x8e) ? skb->len - 14 : skb->len;
			eapol_enqueue(pl, pl_len);
			printk(KERN_INFO "uKernel/net: EAPOL RX (%d bájt payload) a chipről\n", pl_len);
		} else if ((d[0] >> 4) == 4) {   /* IPv4 (DHCP/ICMP) — eth-fejléc lehúzva */
			data_enqueue(0x0800, d, skb->len);
			dhcp_sniff(d, skb->len);
			printk(KERN_INFO "uKernel/net: IP RX (%u bájt) a chipről\n", skb->len);
		} else if (d[0] == 0x00 && d[1] == 0x01 && d[2] == 0x08 && d[3] == 0x00) {   /* ARP */
			data_enqueue(0x0806, d, skb->len);
			/* ARP-válasz (opcode 2) az átjárótól -> a gateway-MAC tanulása (a handlerek
			 * ezzel kihagyják a per-kapcsolat ARP-ot a flaky linken) */
			if (skb->len >= 28 && d[7] == 2) {
				uint32_t sip = ((uint32_t)d[14]<<24)|(d[15]<<16)|(d[16]<<8)|d[17];
				if (g_if_gw && sip == g_if_gw) { memcpy(g_gw_mac, d + 8, 6); g_have_gwmac = 1; }
			}
			printk(KERN_INFO "uKernel/net: ARP RX (%u bájt) a chipről\n", skb->len);
		} else if (d[12] == 0x08 && (d[13] == 0x00 || d[13] == 0x06)) {   /* eth-fejléccel: IP/ARP */
			data_enqueue((d[12] << 8) | d[13], d + 14, skb->len - 14);
			if (d[13] == 0x00) dhcp_sniff(d + 14, skb->len - 14);
			if (d[13] == 0x06 && skb->len >= 14+28 && d[14+7] == 2) {   /* ARP-válasz eth-fejléccel */
				const uint8_t *a = d + 14; uint32_t sip = ((uint32_t)a[14]<<24)|(a[15]<<16)|(a[16]<<8)|a[17];
				if (g_if_gw && sip == g_if_gw) { memcpy(g_gw_mac, a + 8, 6); g_have_gwmac = 1; }
			}
		}
	}
	kfree_skb(skb);
	return 0;
}
int netif_rx(struct sk_buff *skb) { return rx_capture(skb); }
int netif_receive_skb(struct sk_buff *skb) { return rx_capture(skb); }

/* a kapcsolt AP MAC-je (a connect állítja) — az EAPOL-TX eth-fejlécéhez */
static uint8_t g_peer_bssid[6];
void ukernel_netdev_set_peer(const uint8_t *bssid) { if (bssid) memcpy(g_peer_bssid, bssid, 6); }

/* EAPOL TX: a wpa_supplicant a PAYLOAD-ot küldi (SOCK_DGRAM); eth-fejlécet adunk
 * (dst=AP, src=miénk, 0x888E), majd az ndo_start_xmit-en át a chipre. */
int ukernel_netdev_xmit(int idx, const uint8_t *frame, int len)
{
	struct net_device *dev = ukernel_netdev_get(idx);
	if (!dev || !dev->netdev_ops || !dev->netdev_ops->ndo_start_xmit) return -1;
	struct sk_buff *skb = netdev_alloc_skb(dev, len + 14 + 64);
	if (!skb) return -1;
	skb_reserve(skb, 32);
	uint8_t *eth = skb_put(skb, len + 14);
	memcpy(eth, g_peer_bssid, 6);          /* dst = AP BSSID */
	memcpy(eth + 6, dev->dev_addr, 6);     /* src = a mi MAC-ünk */
	eth[12] = 0x88; eth[13] = 0x8e;        /* ethertype EAPOL */
	memcpy(eth + 14, frame, len);          /* payload */
	skb->dev = dev;
	skb->protocol = 0x8e88;
	g_tx_packets++; g_tx_bytes += len + 14;
	printk(KERN_INFO "uKernel/net: EAPOL TX (%d bájt payload -> %d eth) a chipre\n", len, len + 14);
	return dev->netdev_ops->ndo_start_xmit(skb, dev);
}

/* általános TELJES ethernet-keret TX (DHCP/IP) — a hívó adja a teljes eth-keretet */
int ukernel_netdev_xmit_eth(int idx, const uint8_t *eth, int len)
{
	struct net_device *dev = ukernel_netdev_get(idx);
	if (!dev || !dev->netdev_ops || !dev->netdev_ops->ndo_start_xmit) return -1;
	struct sk_buff *skb = netdev_alloc_skb(dev, len + 64);
	if (!skb) return -1;
	skb_reserve(skb, 32);
	memcpy(skb_put(skb, len), eth, len);
	skb->dev = dev;
	skb->protocol = (eth[12] << 8) | eth[13];
	g_tx_packets++; g_tx_bytes += len;
	printk(KERN_INFO "uKernel/net: IP TX (%d bájt eth) a chipre\n", len);
	return dev->netdev_ops->ndo_start_xmit(skb, dev);
}

/* VALÓDI RF-injekció: a nyers [radiotap][802.11] keretet az ndo_start_xmit-re adjuk;
 * monitor-módban a driver ezt a rtw_monitor_xmit_entry-vel a LEVEGŐRE küldi */
int ukernel_netdev_inject(int idx, const uint8_t *frame, int len)
{
	struct net_device *dev = ukernel_netdev_get(idx);
	if (!dev || !dev->netdev_ops || !dev->netdev_ops->ndo_start_xmit) return -1;
	struct sk_buff *skb = netdev_alloc_skb(dev, len + 64);
	if (!skb) return -1;
	skb_reserve(skb, 32);
	memcpy(skb_put(skb, len), frame, len);
	skb->dev = dev;
	skb->protocol = 0x1900;   /* htons(ETH_P_80211_RAW) — csak jelzés */
	g_tx_packets++; g_tx_bytes += len;
	printk(KERN_INFO "uKernel/net: INJEKCIÓ (%d bájt radiotap+802.11) a chipre\n", len);
	return dev->netdev_ops->ndo_start_xmit(skb, dev);
}
