/* uKernel nl80211-bridge — nl80211 parancs-handlerek (moduláris).
 * Minden parancs egy önálló, a táblában regisztrált függvény. A valós adatot
 * (interfész, wiphy, scan-BSS) a uServer proxyból (userver_client) veszi. */
#include "nl_dispatch.h"
#include "uknl_common.h"
#include "userver_client.h"
#include "sysfs.h"
#include <linux/nl80211.h>
#include <string.h>
#include <stdio.h>

/* az aktuális csatorna (freq MHz) és iftype — a GET_INTERFACE ezeket adja vissza
 * (az aireplay/airodump az interfész csatornáját így olvassa; enélkül "channel 0") */
static uint32_t g_cur_freq = 0;
static uint32_t g_cur_iftype = NL80211_IFTYPE_STATION;

/* a dump-üzenetek közös fejléce */
static size_t nl80211_msg(struct nl_buf *b, const struct nl_req *req, uint8_t cmd, int multi)
{
	uint16_t flags = multi ? NLM_F_MULTI : 0;
	return nlmsg_begin(b, UKNL_NL80211_FAMILY_ID, flags,
	                   req->nlh->nlmsg_seq, req->nlh->nlmsg_pid, cmd, 0);
}

/* a VALÓDI interfész-adatok lekérése a uServerből (fallback ha nincs) */
static void get_iface(struct uk_iface_info *ii)
{
	int rc = uk_get_iface(0, ii);
	if (uknl_debug) fprintf(stderr, "[uknl] uk_get_iface(sock=%s) rc=%d name=%s mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
		uk_sock_path(), rc, ii->name, ii->mac[0],ii->mac[1],ii->mac[2],ii->mac[3],ii->mac[4],ii->mac[5]);
	if (rc != 0) {
		memset(ii, 0, sizeof(*ii));
		snprintf(ii->name, sizeof(ii->name), "wlan0");
		ii->ifindex = UKNL_IFINDEX; ii->wiphy_idx = UKNL_WIPHY_IDX;
	}
}

/* ===== NL80211_CMD_GET_INTERFACE (dump) — a VALÓDI wlan0 (név + chip-MAC) ===== */
static int cmd_get_interface(const struct nl_req *req, struct nl_buf *resp)
{
	struct uk_iface_info ii; get_iface(&ii);
	/* az iftype a fake sysfs típusából (803=monitor) — cross-process robusztus */
	uint32_t iftype = g_cur_iftype;
	{ FILE *tf = fopen("/tmp/uksys/sys/class/net/wlan0/type", "r");
	  if (tf) { int tt = 0; if (fscanf(tf, "%d", &tt) == 1 && tt == 803) iftype = NL80211_IFTYPE_MONITOR;
	            else if (tt == 1) iftype = NL80211_IFTYPE_STATION; fclose(tf); } }
	size_t h = nl80211_msg(resp, req, NL80211_CMD_NEW_INTERFACE, 1);
	nla_put_u32(resp, NL80211_ATTR_IFINDEX, (uint32_t)ii.ifindex);
	nla_put_str(resp, NL80211_ATTR_IFNAME, ii.name);
	nla_put_u32(resp, NL80211_ATTR_WIPHY, (uint32_t)ii.wiphy_idx);
	nla_put_u32(resp, NL80211_ATTR_IFTYPE, iftype);
	nla_put(resp, NL80211_ATTR_MAC, ii.mac, 6);
	nla_put_u64(resp, NL80211_ATTR_WDEV, ((uint64_t)ii.wiphy_idx << 32) | 1);
	/* MINDIG küldünk érvényes frekvenciát: ha a chip csatornája ismeretlen (0),
	 * 2412 MHz (1-es csat.) az alap — különben az airodump inicializálatlan értéket
	 * olvas (pl. "CH 33554432"). + csatorna-szélesség (20 MHz) a teljes képhez. */
	uint32_t freq = ii.freq ? ii.freq : (g_cur_freq ? g_cur_freq : 2412);
	nla_put_u32(resp, NL80211_ATTR_WIPHY_FREQ, freq);
	nla_put_u32(resp, NL80211_ATTR_CHANNEL_WIDTH, 0 /*NL80211_CHAN_WIDTH_20_NOHT*/);
	nla_put_u32(resp, NL80211_ATTR_CENTER_FREQ1, freq);
	if (uknl_debug) fprintf(stderr, "[uknl] GET_INTERFACE -> iftype=%u freq=%u (ii.freq=%u)\n", iftype, freq, ii.freq);
	nla_put_u32(resp, NL80211_ATTR_GENERATION, 1);
	nlmsg_end(resp, h);
	nlmsg_put_done(resp, req->nlh->nlmsg_seq, req->nlh->nlmsg_pid);
	return 0;
}

/* ===== NL80211_CMD_GET_WIPHY (dump) — a phy0 sávjai/csatornái ===== */
static void put_band(struct nl_buf *b, int band_idx, const int *freqs, int nfreq, int is_2ghz)
{
	/* támogatott bitráták (100 kbps egység) — enélkül a wpa_supplicant "rate sets
	 * do not match" miatt eldobja az AP-t */
	static const int r24[] = { 10,20,55,110,60,90,120,180,240,360,480,540 }; /* 2.4G: CCK+OFDM */
	static const int r5[]  = { 60,90,120,180,240,360,480,540 };               /* 5G: OFDM */
	const int *rates = is_2ghz ? r24 : r5;
	int nr = is_2ghz ? 12 : 8;

	size_t band = nla_nest_begin(b, band_idx);
	size_t freqs_nest = nla_nest_begin(b, NL80211_BAND_ATTR_FREQS);
	for (int i = 0; i < nfreq; i++) {
		size_t f = nla_nest_begin(b, i + 1);
		nla_put_u32(b, NL80211_FREQUENCY_ATTR_FREQ, freqs[i]);
		nla_nest_end(b, f);
	}
	nla_nest_end(b, freqs_nest);
	size_t rates_nest = nla_nest_begin(b, NL80211_BAND_ATTR_RATES);
	for (int i = 0; i < nr; i++) {
		size_t rt = nla_nest_begin(b, i + 1);
		nla_put_u32(b, NL80211_BITRATE_ATTR_RATE, rates[i]);
		nla_nest_end(b, rt);
	}
	nla_nest_end(b, rates_nest);
	nla_nest_end(b, band);
}
static int cmd_get_wiphy(const struct nl_req *req, struct nl_buf *resp)
{
	static const int g2[] = { 2412,2417,2422,2427,2432,2437,2442,2447,2452,2457,2462,2467,2472,2484 };
	static const int g5[] = { 5180,5200,5220,5240,5260,5280,5300,5320,5500,5520,5540,5560,5580,5600,5620,5640,5660,5680,5700,5745,5765,5785,5805,5825 };
	size_t h = nl80211_msg(resp, req, NL80211_CMD_NEW_WIPHY, 1);
	nla_put_u32(resp, NL80211_ATTR_WIPHY, UKNL_WIPHY_IDX);
	nla_put_str(resp, NL80211_ATTR_WIPHY_NAME, "phy0");
	nla_put_u32(resp, NL80211_ATTR_GENERATION, 1);
	size_t bands = nla_nest_begin(resp, NL80211_ATTR_WIPHY_BANDS);
	put_band(resp, NL80211_BAND_2GHZ, g2, (int)(sizeof(g2)/sizeof(g2[0])), 1);
	put_band(resp, NL80211_BAND_5GHZ, g5, (int)(sizeof(g5)/sizeof(g5[0])), 0);
	nla_nest_end(resp, bands);
	/* támogatott parancsok — a wpa_supplicant ezt nézi (CONNECT-alapú driver) */
	/* CSAK CONNECT/DISCONNECT (NEM AUTH/ASSOC) — így a wpa_supplicant a driver
	 * CONNECT-útját használja (a driver belül auth+assoc), nem a saját SME-jét */
	static const int cmds[] = {
		NL80211_CMD_NEW_KEY, NL80211_CMD_DEL_KEY, NL80211_CMD_SET_KEY,
		NL80211_CMD_CONNECT, NL80211_CMD_DISCONNECT,
		NL80211_CMD_TRIGGER_SCAN, NL80211_CMD_GET_SCAN,
		NL80211_CMD_SET_INTERFACE, NL80211_CMD_FRAME,
	};
	size_t sc = nla_nest_begin(resp, NL80211_ATTR_SUPPORTED_COMMANDS);
	for (int i = 0; i < (int)(sizeof(cmds)/sizeof(cmds[0])); i++)
		nla_put_u32(resp, i + 1, (uint32_t)cmds[i]);
	nla_nest_end(resp, sc);
	/* max scan SSID-k (különben a wpa_supplicant nem scannel aktívan) */
	nla_put_u8(resp, NL80211_ATTR_MAX_NUM_SCAN_SSIDS, 4);
	nlmsg_end(resp, h);
	nlmsg_put_done(resp, req->nlh->nlmsg_seq, req->nlh->nlmsg_pid);
	return 0;
}

/* A scan-eredmények cache-e: a TRIGGER_SCAN azonnal (a scan-flow-ban) lekéri az
 * összes BSS-t a uServerből, így a GET_SCAN már NEM kér a szervertől (nincs
 * verseny a szerver-leállással / külön round-trip). */
#define MAX_CACHE 64
static struct uk_bss_info g_bss_cache[MAX_CACHE];
static int g_bss_cached;

/* ===== NL80211_CMD_TRIGGER_SCAN — valós scan a uServeren át ===== */
static int cmd_trigger_scan(const struct nl_req *req, struct nl_buf *resp)
{
	/* ACK a kérésre — AZONNAL kiküldjük (a scan sokáig tart, iw különben lejár) */
	if (req->nlh->nlmsg_flags & NLM_F_ACK)
		nlmsg_put_ack(resp, req->nlh->nlmsg_seq, req->nlh->nlmsg_pid, 0, req->nlh);
	uknl_send(resp);   /* korai flush: ACK most megy */
	/* A scan-hez a netdev-nek UP-nak kell lennie (ndo_open: RF be + RX-pump).
	 * A guest gyakran scannel `ip link set up` nélkül (vagy az rtnl-up még nem
	 * futott le), ezért itt implicit felhozzuk az interfészt — különben a chip
	 * RF-je ki van kapcsolva és a scan 0 BSS-t ad. Idempotens: ha már UP, no-op. */
	uk_set_ifflags(0, 1);
	/* valós scan (a uServer megvárja a befejezést) */
	uk_scan(UKNL_WIPHY_IDX);
	/* a BSS-eket AZONNAL lekérjük és cache-eljük (amíg a szerver biztosan él) */
	g_bss_cached = 0;
	for (int i = 0; i < MAX_CACHE; i++) {
		if (uk_get_bss(UKNL_WIPHY_IDX, i, &g_bss_cache[g_bss_cached]) != 0) break;
		g_bss_cached++;
	}
	if (uknl_debug) fprintf(stderr, "[uknl] TRIGGER_SCAN: %d BSS cache-elve\n", g_bss_cached);
	/* A scan-eredmény KÉSZ: új generáció. Ezt a setsockopt(ADD_MEMBERSHIP) elkapó
	 * olvassa: ha egy socket a TRIGGER UTÁN iratkozik fel (az `iw scan` pont ezt
	 * teszi — előbb triggerel, aztán subscribe-ol), pótlólag megkapja az eseményt,
	 * különben örökre várna a már kiküldött NEW_SCAN_RESULTS-ra. */
	uknl_scan_gen++;
	/* A NEW_SCAN_RESULTS kézbesítését a handler-szál TÉTLEN ága végzi (uknl_preload.c),
	 * amikor a feliratkozott socket már a recvmsg-ben vár — így az esemény aszinkron,
	 * a kliens-setup UTÁN érkezik, és nem ékelődik a parancs-válasz folyamba. */
	return 0;
}

/* A NEW_SCAN_RESULTS esemény felépítése egy (már inicializált) pufferbe — közös a
 * mcast-kiküldés (TRIGGER_SCAN) és a late-subscriber pótlás (setsockopt) között. */
void uknl_build_scan_event(struct nl_buf *ev)
{
	size_t h = nlmsg_begin(ev, UKNL_NL80211_FAMILY_ID, 0, 0, 0, NL80211_CMD_NEW_SCAN_RESULTS, 0);
	nla_put_u32(ev, NL80211_ATTR_IFINDEX, UKNL_IFINDEX);
	nla_put_u32(ev, NL80211_ATTR_WIPHY, UKNL_WIPHY_IDX);
	nlmsg_end(ev, h);
}

/* ===== NL80211_CMD_GET_SCAN (dump) — a valós BSS-lista ===== */
static int cmd_get_scan(const struct nl_req *req, struct nl_buf *resp)
{
	/* ha nincs cache (pl. `iw scan dump` előzetes trigger nélkül), most scannelünk */
	if (g_bss_cached == 0) {
		uk_set_ifflags(0, 1);   /* RF/RX UP (lásd TRIGGER_SCAN) */
		uk_scan(UKNL_WIPHY_IDX);
		for (int i = 0; i < MAX_CACHE; i++) {
			if (uk_get_bss(UKNL_WIPHY_IDX, i, &g_bss_cache[g_bss_cached]) != 0) break;
			g_bss_cached++;
		}
	}
	if (uknl_debug) fprintf(stderr, "[uknl] cmd_get_scan: %d cache-elt BSS\n", g_bss_cached);
	for (int i = 0; i < g_bss_cached; i++) {
		struct uk_bss_info bi = g_bss_cache[i];

		size_t h = nl80211_msg(resp, req, NL80211_CMD_NEW_SCAN_RESULTS, 1);
		nla_put_u32(resp, NL80211_ATTR_GENERATION, 1);
		nla_put_u32(resp, NL80211_ATTR_IFINDEX, UKNL_IFINDEX);
		size_t bss = nla_nest_begin(resp, NL80211_ATTR_BSS);
		nla_put(resp, NL80211_BSS_BSSID, bi.bssid, 6);
		nla_put_u32(resp, NL80211_BSS_FREQUENCY, (uint32_t)bi.freq);
		nla_put_u32(resp, NL80211_BSS_SIGNAL_MBM, (uint32_t)bi.signal);
		nla_put_u16(resp, NL80211_BSS_CAPABILITY, bi.cap ? bi.cap : 0x0411);
		nla_put_u32(resp, NL80211_BSS_STATUS, 0);
		/* information elements: a VALÓDI beacon-IE-k (RSN/WPA -> iw is mutatja a
		 * titkosítást); fallback: csak a SSID-IE */
		if (bi.ie_len > 0) {
			nla_put(resp, NL80211_BSS_INFORMATION_ELEMENTS, bi.ie, bi.ie_len);
		} else {
			uint8_t ie[2 + 32]; int slen = (int)strnlen(bi.ssid, 32);
			ie[0] = 0; ie[1] = (uint8_t)slen; memcpy(ie + 2, bi.ssid, slen);
			nla_put(resp, NL80211_BSS_INFORMATION_ELEMENTS, ie, 2 + slen);
		}
		nla_nest_end(resp, bss);
		nlmsg_end(resp, h);
	}
	nlmsg_put_done(resp, req->nlh->nlmsg_seq, req->nlh->nlmsg_pid);
	return 0;
}

/* ===== NL80211_CMD_CONNECT — asszociáció (WPA2-PSK) a driveren át ===== */
static int cmd_connect(const struct nl_req *req, struct nl_buf *resp)
{
	struct uk_connect_req cr; memset(&cr, 0, sizeof(cr));
	const struct nlattr *a;
	if ((a = nla_find(req->attrs, req->attrs_len, NL80211_ATTR_SSID))) {
		int sl = a->nla_len - NLA_HDRLEN; if (sl > 32) sl = 32;
		memcpy(cr.ssid, nla_data2(a), sl); cr.ssid_len = (uint8_t)sl;
	}
	if ((a = nla_find(req->attrs, req->attrs_len, NL80211_ATTR_MAC)))
		memcpy(cr.bssid, nla_data2(a), 6);
	if ((a = nla_find(req->attrs, req->attrs_len, NL80211_ATTR_WIPHY_FREQ)))
		cr.freq = *(uint32_t *)nla_data2(a);
	/* a wpa_supplicant assoc-req IE-i (RSN-IE) — ezt a driver az assoc-kérésbe teszi */
	if ((a = nla_find(req->attrs, req->attrs_len, NL80211_ATTR_IE))) {
		int il = a->nla_len - NLA_HDRLEN; if (il > 256) il = 256; if (il < 0) il = 0;
		memcpy(cr.ie, nla_data2(a), il); cr.ie_len = (uint16_t)il;
		if (uknl_debug) fprintf(stderr, "[uknl] CONNECT IE-k: %d bájt\n", il);
	}

	/* ACK azonnal (az asszociáció sokáig tart) */
	if (req->nlh->nlmsg_flags & NLM_F_ACK)
		nlmsg_put_ack(resp, req->nlh->nlmsg_seq, req->nlh->nlmsg_pid, 0, req->nlh);
	uknl_send(resp);

	/* valós asszociáció a driveren/chipen át (blokkol az eredményig) */
	uint8_t bssid[6] = {0};
	int status = uk_connect(UKNL_WIPHY_IDX, &cr, bssid);
	if (uknl_debug) fprintf(stderr, "[uknl] CONNECT '%s' -> status=%d\n", cr.ssid, status);

	/* NL80211_CMD_CONNECT esemény az eredménnyel — az MLME mcast-csoportnak */
	struct nl_buf ev; nlb_init(&ev);
	size_t h = nlmsg_begin(&ev, UKNL_NL80211_FAMILY_ID, 0, 0, 0, NL80211_CMD_CONNECT, 0);
	nla_put_u32(&ev, NL80211_ATTR_IFINDEX, UKNL_IFINDEX);
	nla_put(&ev, NL80211_ATTR_MAC, bssid, 6);
	nla_put_u16(&ev, NL80211_ATTR_STATUS_CODE, (uint16_t)(status >= 0 ? status : 1));
	nlmsg_end(&ev, h);
	uknl_mcast_send(UKNL_MCGRP_MLME, &ev);
	nlb_free(&ev);
	return 0;
}

/* ===== NL80211_CMD_NEW_KEY — a 4-way handshake PTK/GTK telepítése a chipbe ===== */
static int cmd_new_key(const struct nl_req *req, struct nl_buf *resp)
{
	struct uk_key_req kr; memset(&kr, 0, sizeof(kr));
	const struct nlattr *a;
	if (uknl_debug) {  /* a jelenlévő attribútumok dumpja */
		const struct nlattr *p = req->attrs; int rem = req->attrs_len;
		fprintf(stderr, "[uknl] NEW_KEY attrs:");
		while (rem >= (int)NLA_HDRLEN && rem >= p->nla_len) {
			fprintf(stderr, " t%d/l%d", p->nla_type & 0x3fff, p->nla_len - (int)NLA_HDRLEN);
			int al = NLA_ALIGN(p->nla_len); rem -= al; p = (const struct nlattr *)((const char *)p + al);
		}
		fprintf(stderr, "\n");
	}
	/* nested NL80211_ATTR_KEY (a wpa ezt használja): KEY_DATA/IDX/CIPHER/SEQ sub-attrok */
	if ((a = nla_find(req->attrs, req->attrs_len, NL80211_ATTR_KEY))) {
		const struct nlattr *sub = (const struct nlattr *)nla_data2(a);
		int slen = a->nla_len - NLA_HDRLEN;
		const struct nlattr *s;
		if ((s = nla_find(sub, slen, NL80211_KEY_DATA))) {
			int kl = s->nla_len - NLA_HDRLEN; if (kl > 64) kl = 64; if (kl < 0) kl = 0;
			memcpy(kr.key, nla_data2(s), kl); kr.key_len = kl;
		}
		if ((s = nla_find(sub, slen, NL80211_KEY_IDX))) kr.key_idx = *(uint8_t *)nla_data2(s);
		if ((s = nla_find(sub, slen, NL80211_KEY_CIPHER))) kr.cipher = *(uint32_t *)nla_data2(s);
		if ((s = nla_find(sub, slen, NL80211_KEY_SEQ))) {
			int sl = s->nla_len - NLA_HDRLEN; if (sl > 16) sl = 16; if (sl < 0) sl = 0;
			memcpy(kr.seq, nla_data2(s), sl); kr.seq_len = sl;
		}
	}
	if (kr.key_len == 0 && (a = nla_find(req->attrs, req->attrs_len, NL80211_ATTR_KEY_DATA))) {
		int kl = a->nla_len - NLA_HDRLEN; if (kl > 64) kl = 64; if (kl < 0) kl = 0;
		memcpy(kr.key, nla_data2(a), kl); kr.key_len = kl;
	}
	if ((a = nla_find(req->attrs, req->attrs_len, NL80211_ATTR_KEY_IDX)))
		kr.key_idx = *(uint8_t *)nla_data2(a);
	if ((a = nla_find(req->attrs, req->attrs_len, NL80211_ATTR_KEY_CIPHER)))
		kr.cipher = *(uint32_t *)nla_data2(a);
	if ((a = nla_find(req->attrs, req->attrs_len, NL80211_ATTR_KEY_SEQ))) {
		int sl = a->nla_len - NLA_HDRLEN; if (sl > 16) sl = 16; if (sl < 0) sl = 0;
		memcpy(kr.seq, nla_data2(a), sl); kr.seq_len = sl;
	}
	if ((a = nla_find(req->attrs, req->attrs_len, NL80211_ATTR_MAC))) {
		memcpy(kr.mac, nla_data2(a), 6); kr.pairwise = 1;   /* van peer MAC -> pairwise (PTK) */
	}
	int rc = uk_add_key(UKNL_WIPHY_IDX, &kr);
	if (uknl_debug) fprintf(stderr, "[uknl] NEW_KEY idx=%d pairwise=%d cipher=0x%08x len=%d -> %d\n",
		kr.key_idx, kr.pairwise, kr.cipher, kr.key_len, rc);
	nlmsg_put_ack(resp, req->nlh->nlmsg_seq, req->nlh->nlmsg_pid, rc < 0 ? 0 : 0, req->nlh);
	return 0;
}

/* ===== NL80211_CMD_NEW_INTERFACE — monitor-interfész létrehozása (airmon-ng) ===== */
static int cmd_new_interface(const struct nl_req *req, struct nl_buf *resp)
{
	char name[20] = "wlan0mon"; uint32_t iftype = NL80211_IFTYPE_MONITOR;
	const struct nlattr *a;
	if ((a = nla_find(req->attrs, req->attrs_len, NL80211_ATTR_IFNAME))) {
		int l = a->nla_len - NLA_HDRLEN; if (l > 19) l = 19; if (l < 0) l = 0;
		memcpy(name, nla_data2(a), l); name[l] = 0;
	}
	if ((a = nla_find(req->attrs, req->attrs_len, NL80211_ATTR_IFTYPE)))
		iftype = *(uint32_t *)nla_data2(a);
	uknl_sysfs_add_iface(name, iftype == NL80211_IFTYPE_MONITOR ? 803 : 1);
	if (uknl_debug) fprintf(stderr, "[uknl] NEW_INTERFACE '%s' type=%u\n", name, iftype);

	struct uk_iface_info ii; get_iface(&ii);
	size_t h = nl80211_msg(resp, req, NL80211_CMD_NEW_INTERFACE, 0);
	nla_put_u32(resp, NL80211_ATTR_IFINDEX, UKNL_IFINDEX + 1);
	nla_put_str(resp, NL80211_ATTR_IFNAME, name);
	nla_put_u32(resp, NL80211_ATTR_WIPHY, UKNL_WIPHY_IDX);
	nla_put_u32(resp, NL80211_ATTR_IFTYPE, iftype);
	nla_put_u64(resp, NL80211_ATTR_WDEV, ((uint64_t)UKNL_WIPHY_IDX << 32) | 2);
	nla_put(resp, NL80211_ATTR_MAC, ii.mac, 6);
	nlmsg_end(resp, h);
	if (req->nlh->nlmsg_flags & NLM_F_ACK)
		nlmsg_put_ack(resp, req->nlh->nlmsg_seq, req->nlh->nlmsg_pid, 0, req->nlh);
	return 0;
}
/* ===== NL80211_CMD_DEL_INTERFACE — monitor törlése ===== */
static int cmd_del_interface(const struct nl_req *req, struct nl_buf *resp)
{
	/* az ifindexből nem tudjuk a nevet; a feltételezett monitor-neveket töröljük */
	uknl_sysfs_del_iface("wlan0mon");
	if (uknl_debug) fprintf(stderr, "[uknl] DEL_INTERFACE\n");
	nlmsg_put_ack(resp, req->nlh->nlmsg_seq, req->nlh->nlmsg_pid, 0, req->nlh);
	return 0;
}
/* ===== NL80211_CMD_SET_INTERFACE — típus-váltás (set type monitor/managed) ===== */
static int cmd_set_interface(const struct nl_req *req, struct nl_buf *resp)
{
	const struct nlattr *a;
	if ((a = nla_find(req->attrs, req->attrs_len, NL80211_ATTR_IFTYPE))) {
		uint32_t t = *(uint32_t *)nla_data2(a);
		int mon = (t == NL80211_IFTYPE_MONITOR);
		g_cur_iftype = t;   /* a GET_INTERFACE ezt adja (aireplay az iftype-ot olvassa) */
		uknl_sysfs_set_type("wlan0", mon ? 803 : 1);
		extern void uknl_set_monitor_mode(int);
		uknl_set_monitor_mode(mon);                     /* a SIOCGIFHWADDR radiotap-ot adjon */
		int rc = uk_set_monitor(UKNL_WIPHY_IDX, mon);   /* a chipet VALÓBAN monitor-módba */
		if (uknl_debug) fprintf(stderr, "[uknl] SET_INTERFACE type=%u monitor=%d -> chip rc=%d\n", t, mon, rc);
	}
	nlmsg_put_ack(resp, req->nlh->nlmsg_seq, req->nlh->nlmsg_pid, 0, req->nlh);
	return 0;
}

/* ===== NL80211_CMD_SET_CHANNEL / SET_WIPHY (csatorna) — a chip rögzítése ===== */
static int cmd_set_channel(const struct nl_req *req, struct nl_buf *resp)
{
	const struct nlattr *a;
	int freq = 0;
	if ((a = nla_find(req->attrs, req->attrs_len, NL80211_ATTR_WIPHY_FREQ)))
		freq = *(uint32_t *)nla_data2(a);
	if (freq) {
		g_cur_freq = freq;   /* a GET_INTERFACE ezt adja vissza (aireplay csatorna-olvasás) */
		int rc = uk_set_channel(UKNL_WIPHY_IDX, freq);
		if (uknl_debug) fprintf(stderr, "[uknl] SET_CHANNEL freq=%d -> chip rc=%d\n", freq, rc);
	}
	nlmsg_put_ack(resp, req->nlh->nlmsg_seq, req->nlh->nlmsg_pid, 0, req->nlh);
	return 0;
}

/* ===== a parancs-tábla (új parancs = egy sor) ===== */
static const struct nl_cmd nl80211_commands[] = {
	{ NL80211_CMD_SET_CHANNEL,   cmd_set_channel,   "SET_CHANNEL" },
	{ NL80211_CMD_SET_WIPHY,     cmd_set_channel,   "SET_WIPHY" },
	{ NL80211_CMD_GET_INTERFACE, cmd_get_interface, "GET_INTERFACE" },
	{ NL80211_CMD_GET_WIPHY,     cmd_get_wiphy,     "GET_WIPHY" },
	{ NL80211_CMD_TRIGGER_SCAN,  cmd_trigger_scan,  "TRIGGER_SCAN" },
	{ NL80211_CMD_GET_SCAN,      cmd_get_scan,      "GET_SCAN" },
	{ NL80211_CMD_CONNECT,       cmd_connect,       "CONNECT" },
	{ NL80211_CMD_NEW_KEY,       cmd_new_key,       "NEW_KEY" },
	{ NL80211_CMD_NEW_INTERFACE, cmd_new_interface, "NEW_INTERFACE" },
	{ NL80211_CMD_DEL_INTERFACE, cmd_del_interface, "DEL_INTERFACE" },
	{ NL80211_CMD_SET_INTERFACE, cmd_set_interface, "SET_INTERFACE" },
};
static const struct nl_family nl80211_family = {
	.id = UKNL_NL80211_FAMILY_ID, .name = "nl80211",
	.cmds = nl80211_commands, .ncmds = (int)(sizeof(nl80211_commands)/sizeof(nl80211_commands[0])),
};

void nl80211_register(void) { nl_register_family(&nl80211_family); }
