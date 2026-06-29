/* uKernel — cfg80211 mag (külön .so, a shim fölött).
 *
 * A valós wifi driver (pl. rtl8812au) ezen .so szimbólumai ellen linkel:
 * wiphy_new/register, cfg80211_scan_done/inform_bss, ieee80211_get_channel...
 * A scan-eredményeket egy wiphy-regiszterben tartjuk, amit a uServer lekérdez
 * (nl80211 bridge foundation, ld. F7).
 *
 * Betöltési sorrend: libkernel_shim.so (RTLD_GLOBAL) -> cfg80211.so (RTLD_GLOBAL)
 *                    -> wifi_driver.so. */
#include <net/cfg80211.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_WIPHY 8
#define MAX_BSS   64

struct wiphy_reg {
	struct wiphy *w;
	char  name[16];
	struct cfg80211_bss *bss[MAX_BSS];
	int   nbss;
	const struct ieee80211_regdomain *regd;
	struct cfg80211_scan_request *scan_req;
	int   scan_done;
};
static struct wiphy_reg g_wiphy[MAX_WIPHY];
static int g_nwiphy;

static struct wiphy_reg *reg_of(struct wiphy *w)
{ for (int i = 0; i < g_nwiphy; i++) if (g_wiphy[i].w == w) return &g_wiphy[i]; return NULL; }

/* ===== wiphy ===== */
struct wiphy *wiphy_new(const struct cfg80211_ops *ops, int sizeof_priv)
{
	struct wiphy *w = calloc(1, sizeof(*w) + (sizeof_priv > 0 ? sizeof_priv : 0));
	if (!w) return NULL;
	w->ops = ops;
	if (g_nwiphy < MAX_WIPHY) {
		struct wiphy_reg *r = &g_wiphy[g_nwiphy];
		r->w = w; snprintf(r->name, sizeof(r->name), "phy%d", g_nwiphy);
		g_nwiphy++;
	}
	printk(KERN_INFO "uKernel/cfg80211: wiphy_new (priv=%d)\n", sizeof_priv);
	return w;
}
int wiphy_register(struct wiphy *w)
{
	struct wiphy_reg *r = reg_of(w);
	printk(KERN_INFO "uKernel/cfg80211: wiphy_register('%s')\n", r ? r->name : "?");
	/* sávok/csatornák kiírása */
	for (int b = 0; b < NUM_NL80211_BANDS; b++) {
		struct ieee80211_supported_band *sb = w->bands[b];
		if (sb) printk(KERN_INFO "uKernel/cfg80211:   sáv %d: %d csatorna, %d ráta\n",
		               b, sb->n_channels, sb->n_bitrates);
	}
	return 0;
}
void wiphy_unregister(struct wiphy *w) { struct wiphy_reg *r = reg_of(w); printk(KERN_INFO "uKernel/cfg80211: wiphy_unregister('%s')\n", r ? r->name : "?"); }
void wiphy_free(struct wiphy *w) { free(w); }
void wiphy_apply_custom_regulatory(struct wiphy *w, const struct ieee80211_regdomain *regd)
{ struct wiphy_reg *r = reg_of(w); if (r) r->regd = regd; }
int regulatory_hint(struct wiphy *w, const char *alpha2)
{ (void)w; printk(KERN_INFO "uKernel/cfg80211: regulatory_hint('%c%c')\n", alpha2[0], alpha2[1]); return 0; }
void wiphy_rfkill_set_hw_state(struct wiphy *w, bool blocked)
{ (void)w; printk(KERN_INFO "uKernel/cfg80211: rfkill hw_state=%d\n", blocked); }

const char *wiphy_name(struct wiphy *w) { struct wiphy_reg *r = reg_of(w); return r ? r->name : "phy?"; }

struct ieee80211_channel *ieee80211_get_channel(struct wiphy *w, int freq)
{
	for (int b = 0; b < NUM_NL80211_BANDS; b++) {
		struct ieee80211_supported_band *sb = w->bands[b];
		if (!sb) continue;
		for (int i = 0; i < sb->n_channels; i++)
			if (sb->channels[i].center_freq == (u32)freq) return &sb->channels[i];
	}
	return NULL;
}

/* ===== scan / bss ===== */
void cfg80211_scan_done(struct cfg80211_scan_request *request, struct cfg80211_scan_info *info)
{
	for (int i = 0; i < g_nwiphy; i++)
		if (g_wiphy[i].scan_req == request) { g_wiphy[i].scan_done = 1;
			printk(KERN_INFO "uKernel/cfg80211: scan_done('%s') aborted=%d, %d BSS\n",
			       g_wiphy[i].name, info ? info->aborted : 0, g_wiphy[i].nbss); }
}

struct cfg80211_bss *cfg80211_inform_bss(struct wiphy *w, struct ieee80211_channel *chan,
		const u8 *bssid, u64 tsf, u16 cap, u16 beacon_int,
		const u8 *ie, size_t ielen, s32 signal, gfp_t gfp)
{
	(void)tsf; (void)gfp;
	struct wiphy_reg *r = reg_of(w);
	if (!r || r->nbss >= MAX_BSS) return NULL;
	struct cfg80211_bss *bss = calloc(1, sizeof(*bss));
	bss->channel = chan; bss->signal = signal; bss->capability = cap; bss->beacon_interval = beacon_int;
	if (bssid) memcpy(bss->bssid, bssid, 6);
	/* SSID kinyerése az IE-kből (element id 0) */
	if (ie && ielen >= 2 && ie[0] == 0) {
		u8 sl = ie[1] < IEEE80211_MAX_SSID_LEN ? ie[1] : IEEE80211_MAX_SSID_LEN;
		memcpy(bss->ssid, ie + 2, sl); bss->ssid_len = sl;
	}
	/* az IE-ket MÁSOLJUK (a driver buffere nem perzisztens) — a nyers IE-k (RSN/WPA)
	 * kellenek a helyes titkosítás-jelzéshez az airodump/iw számára */
	if (ie && ielen) {
		u8 *cp = malloc(ielen);
		if (cp) { memcpy(cp, ie, ielen); bss->ies = cp; bss->ies_len = ielen; }
	}
	r->bss[r->nbss++] = bss;
	printk(KERN_INFO "uKernel/cfg80211: inform_bss '%.*s' %02x:%02x:%02x:%02x:%02x:%02x sig=%d freq=%u\n",
	       bss->ssid_len, bss->ssid, bssid?bssid[0]:0,bssid?bssid[1]:0,bssid?bssid[2]:0,
	       bssid?bssid[3]:0,bssid?bssid[4]:0,bssid?bssid[5]:0, signal, chan ? chan->center_freq : 0);
	return bss;
}

struct cfg80211_bss *cfg80211_get_bss(struct wiphy *w, struct ieee80211_channel *chan,
		const u8 *bssid, const u8 *ssid, size_t ssid_len, u32 u1, u32 u2)
{
	(void)chan; (void)u1; (void)u2;
	struct wiphy_reg *r = reg_of(w); if (!r) return NULL;
	for (int i = 0; i < r->nbss; i++) {
		if (bssid && memcmp(r->bss[i]->bssid, bssid, 6) == 0) return r->bss[i];
		if (ssid && r->bss[i]->ssid_len == ssid_len && memcmp(r->bss[i]->ssid, ssid, ssid_len) == 0) return r->bss[i];
	}
	return NULL;
}
void cfg80211_put_bss(struct wiphy *w, struct cfg80211_bss *bss) { (void)w; (void)bss; }
void cfg80211_unlink_bss(struct wiphy *w, struct cfg80211_bss *bss)
{
	struct wiphy_reg *r = reg_of(w); if (!r) return;
	for (int i = 0; i < r->nbss; i++) if (r->bss[i] == bss) {
		free((void *)bss->ies); free(bss); for (int j = i; j < r->nbss - 1; j++) r->bss[j] = r->bss[j+1]; r->nbss--; return; }
}

/* ===== uServer lekérdező API (nl80211 bridge foundation) ===== */
struct uk_bss_info { char ssid[33]; uint8_t bssid[6]; int32_t signal; int32_t freq;
	uint16_t cap; uint16_t ie_len; uint8_t ie[256]; };

size_t ukernel_wiphy_count(void) { return g_nwiphy; }
const char *ukernel_wiphy_name(size_t idx) { return idx < (size_t)g_nwiphy ? g_wiphy[idx].name : NULL; }

int ukernel_wiphy_scan(size_t idx)
{
	if (idx >= (size_t)g_nwiphy) return -1;
	struct wiphy_reg *r = &g_wiphy[idx];
	if (!r->w->ops || !r->w->ops->scan) return -2;
	/* régi BSS-ek törlése + scan_request felépítése */
	for (int i = 0; i < r->nbss; i++) { free((void *)r->bss[i]->ies); free(r->bss[i]); }
	r->nbss = 0; r->scan_done = 0;
	/* az ÖSSZES sáv ÖSSZES csatornáját a scan-requestbe (teljes scan) */
	int nch = 0;
	for (int b = 0; b < NUM_NL80211_BANDS; b++)
		if (r->w->bands[b]) nch += r->w->bands[b]->n_channels;
	struct cfg80211_scan_request *req = calloc(1, sizeof(*req) + nch * sizeof(void *));
	req->wiphy = r->w; req->n_ssids = 0; req->n_channels = nch;
	int ci = 0;
	for (int b = 0; b < NUM_NL80211_BANDS; b++) {
		struct ieee80211_supported_band *sb = r->w->bands[b];
		if (!sb) continue;
		for (int i = 0; i < sb->n_channels; i++) req->channels[ci++] = &sb->channels[i];
	}
	/* a scan-opnak a wlan0 wireless_dev-je kell (request->wdev), különben a
	 * driver wdev_to_ndev(NULL)-on elszáll */
	extern struct net_device *ukernel_netdev_for_wiphy(void *wiphy);
	struct net_device *ndev = ukernel_netdev_for_wiphy(r->w);
	if (ndev && ndev->ieee80211_ptr) req->wdev = ndev->ieee80211_ptr;
	r->scan_req = req;
	printk(KERN_INFO "uKernel/cfg80211: ukernel_wiphy_scan('%s') -> ops->scan\n", r->name);
	int rc = r->w->ops->scan(r->w, req);
	/* A valós scan ASZINKRON: a driver kiadja a parancsot a chipnek, a beaconök
	 * URB-eken jönnek vissza, és cfg80211_inform_bss-en át érkeznek; a vége
	 * cfg80211_scan_done. Megvárjuk (max ~8s), hogy összegyűljenek az eredmények. */
	if (rc == 0) {
		for (int i = 0; i < 800 && !r->scan_done; i++) {
			struct timespec ts = { 0, 10 * 1000 * 1000 }; /* 10 ms */
			nanosleep(&ts, NULL);
		}
		printk(KERN_INFO "uKernel/cfg80211: scan vége (done=%d, %d BSS)\n", r->scan_done, r->nbss);
	}
	r->scan_req = NULL; free(req);
	return rc ? rc : r->nbss;
}

size_t ukernel_wiphy_bss_count(size_t idx) { return idx < (size_t)g_nwiphy ? (size_t)g_wiphy[idx].nbss : 0; }

int ukernel_wiphy_bss_get(size_t idx, size_t bi, struct uk_bss_info *out)
{
	if (idx >= (size_t)g_nwiphy) return -1;
	struct wiphy_reg *r = &g_wiphy[idx];
	if (bi >= (size_t)r->nbss) return -1;
	struct cfg80211_bss *b = r->bss[bi];
	memset(out, 0, sizeof(*out));
	memcpy(out->ssid, b->ssid, b->ssid_len);
	memcpy(out->bssid, b->bssid, 6);
	out->signal = b->signal;
	out->freq = b->channel ? b->channel->center_freq : 0;
	out->cap = b->capability;
	/* a VALÓDI beacon-IE-k továbbítása (RSN/WPA -> helyes ENC) */
	if (b->ies && b->ies_len) {
		out->ie_len = b->ies_len > sizeof(out->ie) ? sizeof(out->ie) : b->ies_len;
		memcpy(out->ie, b->ies, out->ie_len);
	}
	return 0;
}

/* ===== cfg80211 event-callbackek (a driver hívja; stub-szint + log) =====
 * Ezek a wiphy/netdev eseményeit jeleznék a userspace (nl80211) felé; egyelőre
 * naplózunk, a tényleges nl80211-bridge-be kötés későbbi lépés. */
/* connect-eredmény elkapása (a driver async hívja a sikeres/sikertelen assoc után) */
static volatile int g_conn_status = -3;   /* -3=nincs folyamatban, -2=folyamatban, >=0=státusz */
static u8 g_conn_bssid[6];

void cfg80211_connect_result(struct net_device *dev, const u8 *bssid, const u8 *req_ie, size_t req_ie_len,
		const u8 *resp_ie, size_t resp_ie_len, u16 status, gfp_t gfp)
{ (void)dev;(void)req_ie;(void)req_ie_len;(void)resp_ie;(void)resp_ie_len;(void)gfp;
  if (bssid) memcpy(g_conn_bssid, bssid, 6);
  g_conn_status = status;
  printk(KERN_INFO "uKernel/cfg80211: connect_result status=%u bssid=%02x:%02x:%02x:%02x:%02x:%02x\n",
         status, bssid?bssid[0]:0,bssid?bssid[1]:0,bssid?bssid[2]:0,bssid?bssid[3]:0,bssid?bssid[4]:0,bssid?bssid[5]:0); }
void cfg80211_connect_bss(struct net_device *dev, const u8 *bssid, struct cfg80211_bss *bss,
		const u8 *req_ie, size_t req_ie_len, const u8 *resp_ie, size_t resp_ie_len,
		int status, gfp_t gfp, int timeout_reason)
{ (void)dev;(void)bss;(void)req_ie;(void)req_ie_len;(void)resp_ie;(void)resp_ie_len;(void)gfp;(void)timeout_reason;
  if (bssid) memcpy(g_conn_bssid, bssid, 6);
  g_conn_status = status;
  printk(KERN_INFO "uKernel/cfg80211: connect_bss status=%d\n", status); }
void cfg80211_roamed(struct net_device *dev, struct cfg80211_roam_info *info, gfp_t gfp)
{ (void)dev;(void)info;(void)gfp; g_conn_status = 0; printk(KERN_INFO "uKernel/cfg80211: roamed\n"); }

/* uServer-API: asszociáció a megadott hálózathoz (WPA2-PSK). Visszaadja a connect
 * státuszt (0=siker), vagy -2 ha timeout/nincs connect-op. */
int ukernel_wiphy_connect(size_t idx, const char *ssid, int ssid_len, const uint8_t *bssid, int freq,
                          const uint8_t *ie, int ie_len)
{
	if (idx >= (size_t)g_nwiphy) return -1;
	struct wiphy_reg *r = &g_wiphy[idx];
	if (!r->w->ops || !r->w->ops->connect) return -2;
	extern struct net_device *ukernel_netdev_for_wiphy(void *wiphy);
	struct net_device *ndev = ukernel_netdev_for_wiphy(r->w);
	if (!ndev || !ssid) return -1;

	static u8 ssidbuf[33], bssidbuf[6];
	struct cfg80211_connect_params sme;
	memset(&sme, 0, sizeof(sme));
	if (ssid_len < 0) ssid_len = 0;   /* negatív hossz elleni védelem (memcpy size_t) */
	if (ssid_len > 32) ssid_len = 32;
	memcpy(ssidbuf, ssid, ssid_len);
	sme.ssid = ssidbuf; sme.ssid_len = ssid_len;
	if (bssid && (bssid[0]|bssid[1]|bssid[2]|bssid[3]|bssid[4]|bssid[5])) {
		memcpy(bssidbuf, bssid, 6); sme.bssid = bssidbuf;
		/* az AP MAC-je az EAPOL-TX eth-fejlécéhez */
		extern void ukernel_netdev_set_peer(const uint8_t *);
		ukernel_netdev_set_peer(bssid);
	}
	if (freq) sme.channel = ieee80211_get_channel(r->w, freq);
	sme.auth_type = NL80211_AUTHTYPE_OPEN_SYSTEM;
	sme.privacy = true;
	sme.crypto.wpa_versions = NL80211_WPA_VERSION_2;
	sme.crypto.cipher_group = 0x000FAC04;          /* CCMP */
	sme.crypto.n_ciphers_pairwise = 1; sme.crypto.ciphers_pairwise[0] = 0x000FAC04;
	sme.crypto.n_akm_suites = 1; sme.crypto.akm_suites[0] = 0x000FAC02;  /* PSK */
	/* a wpa_supplicant PONTOS assoc-req IE-i (RSN-IE) — a driver ezt teszi az
	 * assoc-kérésbe (rtw_cfg80211_set_wpa_ie); enélkül a security nincs jól beállítva */
	static uint8_t iebuf[256];
	if (ie && ie_len > 0) {
		if (ie_len > 256) ie_len = 256;
		memcpy(iebuf, ie, ie_len);
		sme.ie = iebuf; sme.ie_len = ie_len;
		printk(KERN_INFO "uKernel/cfg80211: connect IE-k: %d bájt\n", ie_len);
	}

	g_conn_status = -2;   /* folyamatban */
	printk(KERN_INFO "uKernel/cfg80211: ukernel_wiphy_connect('%.*s') -> ops->connect\n", ssid_len, ssid);
	int rc = r->w->ops->connect(r->w, ndev, &sme);
	if (rc) { printk(KERN_INFO "uKernel/cfg80211: ops->connect azonnal hibázott: %d\n", rc); return rc; }
	/* a driver async asszociál (auth+assoc a chipen); várjuk a connect_result-ot */
	for (int i = 0; i < 1500 && g_conn_status == -2; i++) {
		struct timespec ts = { 0, 10 * 1000 * 1000 }; nanosleep(&ts, NULL);  /* 10 ms, max 15s */
	}
	printk(KERN_INFO "uKernel/cfg80211: connect vége, status=%d\n", g_conn_status);
	return g_conn_status == -2 ? -110 /*ETIMEDOUT*/ : g_conn_status;
}
int ukernel_wiphy_conn_bssid(uint8_t *out) { memcpy(out, g_conn_bssid, 6); return g_conn_status; }

/* a chip VALÓDI monitor-módba kapcsolása (change_virtual_intf) — enélkül a chip
 * nem fogad minden keretet, és az injekció (rtw_monitor_xmit_entry) sem aktív */
int ukernel_wiphy_set_monitor(size_t idx, int enable)
{
	if (idx >= (size_t)g_nwiphy) return -1;
	struct wiphy_reg *r = &g_wiphy[idx];
	if (!r->w->ops || !r->w->ops->change_virtual_intf) return -2;
	extern struct net_device *ukernel_netdev_for_wiphy(void *wiphy);
	struct net_device *ndev = ukernel_netdev_for_wiphy(r->w);
	struct vif_params params; memset(&params, 0, sizeof(params));
	enum nl80211_iftype t = enable ? NL80211_IFTYPE_MONITOR : NL80211_IFTYPE_STATION;
	printk(KERN_INFO "uKernel/cfg80211: change_virtual_intf -> %s\n", enable ? "MONITOR" : "STATION");
	int (*ci)(struct wiphy *, struct net_device *, enum nl80211_iftype, struct vif_params *)
		= (void *)r->w->ops->change_virtual_intf;
	return ci(r->w, ndev, t, &params);
}

/* a chip aktuális csatornája (a set_channel állítja) — a GET_IFACE ezt adja vissza */
static int g_cur_chan_freq = 0;
int ukernel_wiphy_get_chan_freq(void) { return g_cur_chan_freq; }

/* a chip RÖGZÍTÉSE egy csatornára (set_monitor_channel) — hogy a monitor-injekció és
 * -vétel pontosan az AP csatornáján legyen (a response rate-hez kulcs) */
int ukernel_wiphy_set_channel(size_t idx, int freq)
{
	g_cur_chan_freq = freq;
	if (idx >= (size_t)g_nwiphy) return -1;
	struct wiphy_reg *r = &g_wiphy[idx];
	if (!r->w->ops || !r->w->ops->set_monitor_channel) return -2;
	static struct ieee80211_channel chan;   /* static: a driver megtarthatja a ptr-t */
	memset(&chan, 0, sizeof(chan));
	chan.band = NL80211_BAND_2GHZ;
	chan.center_freq = freq;
	chan.hw_value = (freq == 2484) ? 14 : (freq - 2407) / 5;
	struct cfg80211_chan_def cd; memset(&cd, 0, sizeof(cd));
	cd.chan = &chan; cd.width = NL80211_CHAN_WIDTH_20; cd.center_freq1 = freq;
	printk(KERN_INFO "uKernel/cfg80211: set_monitor_channel freq=%d ch=%d\n", freq, chan.hw_value);
	int (*smc)(struct wiphy *, struct cfg80211_chan_def *) = (void *)r->w->ops->set_monitor_channel;
	return smc(r->w, &cd);
}

/* a 4-way handshake utáni kulcsok (PTK/GTK) telepítése a driverbe/chipbe — enélkül
 * az adat-keretek nem titkosíthatók/dekódolhatók (a DHCP/IP elveszik) */
int ukernel_wiphy_add_key(size_t idx, int key_idx, int pairwise, const uint8_t *mac,
                          uint32_t cipher, const uint8_t *key, int key_len,
                          const uint8_t *seq, int seq_len)
{
	if (idx >= (size_t)g_nwiphy) return -1;
	struct wiphy_reg *r = &g_wiphy[idx];
	if (!r->w->ops || !r->w->ops->add_key) return -2;
	/* a hossz-mezők korlátozása a forrás-puffer méreteihez (over-read ellen):
	 * a uk_key_req key[64], seq[16] — a driver ezeken túl nem olvashat */
	if (key_len < 0) key_len = 0; if (key_len > 64) key_len = 64;
	if (seq_len < 0) seq_len = 0; if (seq_len > 16) seq_len = 16;
	extern struct net_device *ukernel_netdev_for_wiphy(void *wiphy);
	struct net_device *ndev = ukernel_netdev_for_wiphy(r->w);
	struct key_params kp; memset(&kp, 0, sizeof(kp));
	kp.key = key; kp.key_len = key_len; kp.seq = seq; kp.seq_len = seq_len; kp.cipher = cipher;
	const uint8_t *macp = (mac && (mac[0]|mac[1]|mac[2]|mac[3]|mac[4]|mac[5])) ? mac : NULL;
	printk(KERN_INFO "uKernel/cfg80211: add_key idx=%d pairwise=%d cipher=0x%08x len=%d\n",
	       key_idx, pairwise, cipher, key_len);
	/* a driver a 2. argumentumként net_device-t vár (mint a connect-nél) */
	int (*ak)(struct wiphy *, struct net_device *, int, u8, bool, const u8 *, struct key_params *)
		= (void *)r->w->ops->add_key;
	int rc = ak(r->w, ndev, 0 /*link_id*/, (u8)key_idx, pairwise ? true : false, macp, &kp);
	/* a páros (PTK) kulcs telepítése = sikeres 4-way -> a link él (carrier on) */
	if (pairwise && rc == 0) { extern void ukernel_netdev_set_carrier(int); ukernel_netdev_set_carrier(1); }
	return rc;
}
void cfg80211_disconnected(struct net_device *dev, u16 reason, const u8 *ie, size_t ie_len, bool locally, gfp_t gfp)
{ (void)dev;(void)ie;(void)ie_len;(void)locally;(void)gfp; extern void ukernel_netdev_set_carrier(int); ukernel_netdev_set_carrier(0); printk(KERN_INFO "uKernel/cfg80211: disconnected reason=%u\n", reason); }
void cfg80211_ibss_joined(struct net_device *dev, const u8 *bssid, struct ieee80211_channel *chan, gfp_t gfp)
{ (void)dev;(void)bssid;(void)chan;(void)gfp; printk(KERN_INFO "uKernel/cfg80211: ibss_joined\n"); }
void cfg80211_rx_mgmt(struct wireless_dev *wdev, int freq, int sig, const u8 *buf, size_t len, u32 flags)
{ (void)wdev;(void)freq;(void)sig;(void)buf;(void)len;(void)flags; }
void cfg80211_mgmt_tx_status(struct wireless_dev *wdev, u64 cookie, const u8 *buf, size_t len, bool ack, gfp_t gfp)
{ (void)wdev;(void)cookie;(void)buf;(void)len;(void)ack;(void)gfp; }
void cfg80211_ready_on_channel(struct wireless_dev *wdev, u64 cookie, struct ieee80211_channel *chan, unsigned int duration, gfp_t gfp)
{ (void)wdev;(void)cookie;(void)chan;(void)duration;(void)gfp; }
void cfg80211_remain_on_channel_expired(struct wireless_dev *wdev, u64 cookie, struct ieee80211_channel *chan, gfp_t gfp)
{ (void)wdev;(void)cookie;(void)chan;(void)gfp; }
void cfg80211_michael_mic_failure(struct net_device *dev, const u8 *addr, int key_type, int key_id, const u8 *tsc, gfp_t gfp)
{ (void)dev;(void)addr;(void)key_type;(void)key_id;(void)tsc;(void)gfp; printk(KERN_INFO "uKernel/cfg80211: michael_mic_failure\n"); }
void cfg80211_cqm_rssi_notify(struct net_device *dev, int event, s32 level, gfp_t gfp)
{ (void)dev;(void)event;(void)level;(void)gfp; }
void cfg80211_ch_switch_notify(struct net_device *dev, struct cfg80211_chan_def *chandef, unsigned int link_id, u16 punct_bitmap)
{ (void)dev;(void)chandef;(void)link_id;(void)punct_bitmap; }
void cfg80211_ch_switch_started_notify(struct net_device *dev, struct cfg80211_chan_def *chandef, unsigned int link_id, u8 count, bool quiet, u16 punct_bitmap)
{ (void)dev;(void)chandef;(void)link_id;(void)count;(void)quiet;(void)punct_bitmap; }
void cfg80211_sched_scan_results(struct wiphy *wiphy, u64 reqid) { (void)wiphy;(void)reqid; }
void cfg80211_unregister_wdev(struct wireless_dev *wdev) { (void)wdev; }
int  cfg80211_sinfo_alloc_tid_stats(struct station_info *sinfo, gfp_t gfp) { (void)sinfo;(void)gfp; return 0; }
void cfg80211_new_sta(struct net_device *dev, const u8 *mac, struct station_info *sinfo, gfp_t gfp)
{ (void)dev;(void)mac;(void)sinfo;(void)gfp; printk(KERN_INFO "uKernel/cfg80211: new_sta\n"); }
void cfg80211_del_sta(struct net_device *dev, const u8 *mac, gfp_t gfp) { (void)dev;(void)mac;(void)gfp; }
void cfg80211_external_auth_request(struct net_device *dev, void *params, gfp_t gfp) { (void)dev;(void)params;(void)gfp; }
struct cfg80211_bss *cfg80211_inform_bss_frame(struct wiphy *wiphy, struct ieee80211_channel *chan,
		struct ieee80211_mgmt *mgmt, size_t len, s32 signal, gfp_t gfp)
{
	/* beacon/probe-resp keret: [24B 802.11 hdr][8B tsf][2B beacon_int][2B cap][IE-k...]
	 * BSSID = addr3 (hdr offset 16). Az IE-k offset 36-tól (id 0 = SSID). */
	const u8 *f = (const u8 *)mgmt;
	if (!f || len < 36) return cfg80211_inform_bss(wiphy, chan, NULL, 0, 0, 0, NULL, 0, signal, gfp);
	const u8 *bssid = f + 16;
	u16 beacon_int = f[32] | (f[33] << 8);
	u16 cap = f[34] | (f[35] << 8);
	const u8 *ie = f + 36;
	size_t ielen = len - 36;
	return cfg80211_inform_bss(wiphy, chan, bssid, 0, cap, beacon_int, ie, ielen, signal, gfp);
}
void cfg80211_send_rx_assoc(struct net_device *dev, struct cfg80211_bss *bss, const u8 *buf, size_t len)
{ (void)dev;(void)bss;(void)buf;(void)len; }
void cfg80211_send_disassoc(struct net_device *dev, const u8 *buf, size_t len) { (void)dev;(void)buf;(void)len; }
struct ieee80211_channel *ieee80211_get_channel_khz(struct wiphy *wiphy, u32 freq) { return ieee80211_get_channel(wiphy, freq/1000); }
