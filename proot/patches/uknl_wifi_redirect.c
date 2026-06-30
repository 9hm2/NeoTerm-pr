/* NeoTerm USB Wi-Fi (uKernel framework) — proot control-plane redirect, gated by
 * UK_WIFI. Injected into syscall/enter.c after the USB redirect; its dispatch is
 * called right after uknl_usb_dispatch.
 *
 * W3a (this file): make the guest's standard kernel-module commands drive the
 * app-side ukwifid daemon over the abstract socket @io.neoterm.wifi:
 *   - finit_module(fd)        -> UK_OP_MODPROBE <name>   (name from the fd path)
 *   - init_module(buf,len)    -> UK_OP_MODPROBE <name>   (name from .modinfo "name=")
 *   - delete_module(name)     -> UK_OP_RMMOD <name>
 * The daemon dlopens/closes the matching pre-built vendor driver .so and probes
 * the chip (over io.neoterm.usb), and writes /proc/modules + /sys/class/net.
 * `lsmod` reads the bound /proc/modules file (written by the daemon) — no syscall
 * redirect needed for it.
 *
 * W3b (later): AF_NETLINK(nl80211+rtnetlink)/AF_PACKET/wext ferrying for
 * iw/wpa_supplicant.
 *
 * Not compiled standalone: uses enter.c's Tracee, peek_reg/poke_reg, read_data,
 * read_string, readlink_proc_pid_fd, set_sysnum(PR_void), PR_*, SYSARG_*, CURRENT.
 */
static int g_uk_wifi = -1;
static uint32_t g_wext_mode = 0;   /* last WEXT SIOCSIWMODE (6=monitor, 2=managed) */
static int uk_wifi_on(void)
{
	if (g_uk_wifi < 0) { const char *e = getenv("UK_WIFI"); g_uk_wifi = (e && *e && *e != '0') ? 1 : 0; }
	return g_uk_wifi;
}

/* Redirect-side trace: append to $UK_WIFI_REDIR_LOG (a guest-readable file under
 * the app data dir, set by ProotManager). strace can't run under proot's nested
 * ptrace, so this is how we see the ferry's recvmsg/sendmsg decisions. */
#include <stdarg.h>
#include <fcntl.h>
static void ukw_dlog(const char *fmt, ...)
{
	static int fd = -2;
	if (fd == -2) { const char *p = getenv("UK_WIFI_REDIR_LOG"); fd = (p && *p) ? open(p, O_WRONLY | O_CREAT | O_APPEND, 0600) : -1; }
	if (fd < 0) return;
	char b[400]; va_list a; va_start(a, fmt); int n = vsnprintf(b, sizeof b, fmt, a); va_end(a);
	if (n > 0) { if (n >= (int) sizeof b) n = (int) sizeof b - 1; ssize_t w = write(fd, b, (size_t) n); (void) w; }
}

/* wire protocol — mirrors app/src/main/cpp/ukfs/include/ukernel/proxy.h */
struct ukw_req { uint32_t op, cmd, len; };
struct ukw_rsp { int32_t ret; uint32_t len; };
#define UKW_OP_EAPOL_TX  15
#define UKW_OP_EAPOL_RX  16
#define UKW_OP_INJECT    21
#define UKW_OP_MONITOR_RX 22
#define UKW_OP_MODPROBE 30
#define UKW_OP_RMMOD    31
#define UKW_OP_NL       33
#define UKW_OP_RTNL     36

/* Connect to the daemon's abstract socket @io.neoterm.wifi. */
static int ukw_connect(void)
{
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s < 0) return -1;
	struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX;
	static const char nm[] = "io.neoterm.wifi";
	a.sun_path[0] = '\0'; memcpy(a.sun_path + 1, nm, sizeof(nm) - 1);
	socklen_t L = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + (sizeof(nm) - 1));
	if (connect(s, (struct sockaddr *)&a, L) < 0) { close(s); return -1; }
	return s;
}

/* Send one op + name payload; return the daemon's rsp.ret (<0 on transport err). */
static int ukw_call(uint32_t op, const char *name)
{
	int s = ukw_connect(); if (s < 0) return -1;
	uint32_t nl = (uint32_t) strlen(name);
	struct ukw_req req = { op, 0, nl };
	int rc = -1;
	if (write(s, &req, sizeof req) == (ssize_t) sizeof req &&
	    (nl == 0 || write(s, name, nl) == (ssize_t) nl)) {
		struct ukw_rsp rsp;
		ssize_t n = read(s, &rsp, sizeof rsp);
		if (n == (ssize_t) sizeof rsp) rc = rsp.ret;
	}
	close(s);
	return rc;
}

/* Send a raw netlink request (UK_OP_NL) and read the raw reply; returns reply
 * length, or <0 on transport error. */
static int ukw_nl_call(uint32_t op, const uint8_t *req, int reqlen, uint8_t *out, int outcap)
{
	int s = ukw_connect(); if (s < 0) return -1;
	struct ukw_req r = { op, 0, (uint32_t) reqlen };
	int rc = -1;
	if (write(s, &r, sizeof r) == (ssize_t) sizeof r &&
	    (reqlen == 0 || write(s, req, (size_t) reqlen) == (ssize_t) reqlen)) {
		struct ukw_rsp rs;
		if (read(s, &rs, sizeof rs) == (ssize_t) sizeof rs) {
			int len = (int) rs.len; if (len > outcap) len = outcap;
			int got = 0;
			while (got < len) { ssize_t k = read(s, out + got, (size_t)(len - got)); if (k <= 0) break; got += (int) k; }
			rc = got;
		}
	}
	close(s);
	return rc;
}

/* AF_PACKET TX: send a frame to the chip (EAPOL or monitor inject); returns the
 * daemon's ret. */
static int ukw_tx(uint32_t op, uint32_t cmd, const uint8_t *frame, int len)
{
	int s = ukw_connect(); if (s < 0) return -1;
	struct ukw_req r = { op, cmd, (uint32_t) len };
	int rc = -1;
	if (write(s, &r, sizeof r) == (ssize_t) sizeof r &&
	    (len <= 0 || write(s, frame, (size_t) len) == (ssize_t) len)) {
		struct ukw_rsp rs; if (read(s, &rs, sizeof rs) == (ssize_t) sizeof rs) rc = rs.ret;
	}
	close(s); return rc;
}

/* AF_PACKET RX: fetch the next pending frame (EAPOL or monitor); 0 = none. */
static int ukw_rx(uint32_t op, uint32_t cmd, uint8_t *out, int outcap)
{
	int s = ukw_connect(); if (s < 0) return 0;
	struct ukw_req r = { op, cmd, 0 };
	int got = 0;
	if (write(s, &r, sizeof r) == (ssize_t) sizeof r) {
		struct ukw_rsp rs;
		if (read(s, &rs, sizeof rs) == (ssize_t) sizeof rs) {
			int len = (int) rs.len; if (len > outcap) len = outcap;
			while (got < len) { ssize_t k = read(s, out + got, (size_t)(len - got)); if (k <= 0) break; got += (int) k; }
		}
	}
	close(s); return got;
}

/* Current scan generation (side-effect-free) — for poll readiness probing. */
static unsigned ukw_scangen(void)
{
	int s = ukw_connect(); if (s < 0) return 0;
	struct ukw_req r = { 35 /* UK_OP_NL_SCANGEN */, 0, 0 };
	unsigned g = 0;
	if (write(s, &r, sizeof r) == (ssize_t) sizeof r) {
		struct ukw_rsp rs; if (read(s, &rs, sizeof rs) == (ssize_t) sizeof rs) g = (unsigned) rs.ret;
	}
	close(s); return g;
}

/* Fetch a pending async event for last_gen; *cur := current gen; returns event
 * length (0 = none). UK_OP_NL_EVENT. */
static int ukw_event(unsigned last_gen, unsigned *cur, uint8_t *out, int outcap)
{
	int s = ukw_connect(); if (s < 0) return 0;
	struct ukw_req r = { 34 /* UK_OP_NL_EVENT */, last_gen, 0 };
	int n = 0;
	if (write(s, &r, sizeof r) == (ssize_t) sizeof r) {
		struct ukw_rsp rs;
		if (read(s, &rs, sizeof rs) == (ssize_t) sizeof rs) {
			if (cur) *cur = (unsigned) rs.ret;
			int len = (int) rs.len; if (len > outcap) len = outcap;
			int got = 0;
			while (got < len) { ssize_t k = read(s, out + got, (size_t)(len - got)); if (k <= 0) break; got += (int) k; }
			n = got;
		}
	}
	close(s); return n;
}

/* Fetch the chip MAC (and presence) from the daemon (UK_OP_GET_IFACE). mac[6] is
 * zeroed on failure. uk_iface_info layout: name[16], mac[6] at offset 16. */
static int ukw_iface_mac(uint8_t mac[6])
{
	memset(mac, 0, 6);
	int s = ukw_connect(); if (s < 0) return -1;
	struct ukw_req r = { 13 /* UK_OP_GET_IFACE */, 0 /* netdev idx 0 = wlan0 */, 0 };
	int rc = -1;
	if (write(s, &r, sizeof r) == (ssize_t) sizeof r) {
		struct ukw_rsp rs;
		if (read(s, &rs, sizeof rs) == (ssize_t) sizeof rs && rs.ret == 0 && rs.len >= 22) {
			uint8_t info[256]; int len = (int) rs.len; if (len > (int) sizeof info) len = sizeof info;
			int got = 0;
			while (got < len) { ssize_t k = read(s, info + got, (size_t)(len - got)); if (k <= 0) break; got += (int) k; }
			if (got >= 22) { memcpy(mac, info + 16, 6); rc = 0; }
		}
	}
	close(s);
	return rc;
}

/* Per-(tgid,genl-fd) state: a guest nl80211/genl netlink socket whose sendmsg is
 * ferried to the daemon (UK_OP_NL) and whose reply is drained by recvmsg. */
struct wnl_fd { int used, pid, fd; uint8_t *reply; int rlen, roff; int sub; unsigned last_gen;
                int is_packet, pkt_eapol;   /* AF_PACKET: EAPOL (0x888e) vs monitor (ETH_P_ALL) */
                int is_route;               /* AF_NETLINK NETLINK_ROUTE (ip link/addr) */
                uint32_t fake_port; };      /* rtnl: deterministic local nl_pid (getsockname + reply stamp) */
#define WNL_MAX 32
static struct wnl_fd g_wnl[WNL_MAX];
static int g_nwnl;
static struct wnl_fd *wnl_get(int pid, int fd, int create)
{
	for (int i = 0; i < g_nwnl; i++)
		if (g_wnl[i].used && g_wnl[i].pid == pid && g_wnl[i].fd == fd) return &g_wnl[i];
	if (!create) return NULL;
	int slot = -1;
	for (int i = 0; i < g_nwnl; i++) if (!g_wnl[i].used) { slot = i; break; }
	if (slot < 0) { if (g_nwnl >= WNL_MAX) return NULL; slot = g_nwnl++; }
	struct wnl_fd *w = &g_wnl[slot];
	memset(w, 0, sizeof *w); w->used = 1; w->pid = pid; w->fd = fd;
	return w;
}
static void wnl_free(struct wnl_fd *w) { if (w) { free(w->reply); memset(w, 0, sizeof *w); } }

/* Deliver exactly ONE captured frame for a marked AF_PACKET socket into the
 * tracee buffer (dst,cap). aircrack/airodump read() one radiotap frame per call,
 * so the daemon's MONITOR_RX batch ([len LE16][frame]..) must be split here; EAPOL
 * RX is a single frame. Returns bytes written, or -EAGAIN when nothing is pending.
 * The whole frame is consumed even if the caller's buffer truncated it. */
static int ukw_packet_recv_one(Tracee *tracee, struct wnl_fd *w, word_t dst, int cap)
{
	if (!(w->reply && w->roff < w->rlen)) {
		free(w->reply); w->reply = NULL; w->rlen = 0; w->roff = 0;
		uint8_t fr[8192];
		int fl = ukw_rx(w->pkt_eapol ? UKW_OP_EAPOL_RX : UKW_OP_MONITOR_RX, 0, fr, sizeof fr);
		if (fl <= 0) return -EAGAIN;
		w->reply = (uint8_t *) malloc(fl);
		if (!w->reply) return -EAGAIN;
		memcpy(w->reply, fr, fl); w->rlen = fl; w->roff = 0;
	}
	int avail = w->rlen - w->roff, flen, hdr;
	if (w->pkt_eapol) { flen = avail; hdr = 0; }            /* EAPOL: stash is one frame */
	else {                                                   /* MONITOR: [len LE16][frame] */
		if (avail < 2) { w->roff = w->rlen; return -EAGAIN; }
		flen = w->reply[w->roff] | (w->reply[w->roff + 1] << 8); hdr = 2;
		if (flen <= 0 || hdr + flen > avail) { w->roff = w->rlen; return -EAGAIN; }
	}
	int n = cap < flen ? cap : flen;
	if (n > 0 && write_data(tracee, dst, w->reply + w->roff + hdr, (word_t) n) < 0) return -EAGAIN;
	w->roff += hdr + flen;
	return n;
}

/* Mark a genl (NETLINK_GENERIC) socket at socket() EXIT so its sendmsg/recvmsg
 * get ferried. Called from translate_syscall_exit. Only NETLINK_GENERIC (nl80211)
 * — NETLINK_ROUTE/uevent are left alone (rtnetlink = a later step; the libusb
 * uevent socket is the USB shim's). */
void uknl_wifi_mark_socket(Tracee *tracee)
{
	if (!uk_wifi_on()) return;
	if (get_sysnum(tracee, ORIGINAL) != PR_socket) return;
	int domain = (int) peek_reg(tracee, ORIGINAL, SYSARG_1);
	int proto  = (int) peek_reg(tracee, ORIGINAL, SYSARG_3);
	int fd = (int) peek_reg(tracee, CURRENT, SYSARG_RESULT);
	if (fd < 0) return;
	if (domain == 16 /* AF_NETLINK */ && proto == 16 /* NETLINK_GENERIC */) {
		wnl_get(ukfs_tgid(tracee->pid), fd, 1);   /* nl80211 genl socket */
		return;
	}
	if (domain == 16 /* AF_NETLINK */ && proto == 0 /* NETLINK_ROUTE */) {
		struct wnl_fd *w = wnl_get(ukfs_tgid(tracee->pid), fd, 1);   /* rtnetlink (ip) */
		/* Deterministic local port: iproute2 reads its bound port via getsockname
		 * and then ACCEPTS a reply only if nlmsg_pid == that port. We can't query
		 * the guest's real socket from the tracer, so we hand out a fixed fake port
		 * (per slot) via the getsockname intercept and stamp every rtnl reply with
		 * the same value so they always match. (Like the uKernel backup's 0x1000+idx.) */
		if (w) { w->is_route = 1; w->fake_port = 0x4000u + (uint32_t)(w - g_wnl); }
		return;
	}
	if (domain == 17 /* AF_PACKET */) {
		/* wpa's EAPOL socket (protocol htons(ETH_P_PAE=0x888e)) vs a monitor/raw
		 * socket (htons(ETH_P_ALL) etc.). protocol is passed network-order. */
		int be = ((proto & 0xff) << 8) | ((proto >> 8) & 0xff);
		struct wnl_fd *w = wnl_get(ukfs_tgid(tracee->pid), fd, 1);
		if (w) { w->is_packet = 1; w->pkt_eapol = (be == 0x888e); }
		return;
	}
}

/* basename of a module path/fd-link -> bare module name: strip dirs, a leading
 * "memfd:" (kmod decompresses into a memfd named after the module), and a
 * trailing ".ko"/".ko.xz"/".ko.zst"/".ko.gz". */
static void ukw_modname(const char *path, char *out, size_t osz)
{
	const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
	if (strncmp(b, "memfd:", 6) == 0) b += 6;
	size_t n = 0;
	for (; b[n] && n < osz - 1; n++) {
		if (b[n] == '.' && (b[n+1] == 'k' && b[n+2] == 'o')) break;   /* stop at ".ko" */
		out[n] = b[n];
	}
	out[n] = '\0';
}

#define UKW_RET(v) do { poke_reg(tracee, SYSARG_RESULT, (word_t)(long)(v)); set_sysnum(tracee, PR_void); return true; } while (0)

/* Ferry a complete netlink request (flat buffer) to the daemon and stash the
 * reply for recvmsg/recvfrom delivery. Shared by sendmsg (iov-gathered) and
 * sendto (flat). Handles the rtnl reply pid-stamp and TRIGGER_SCAN auto-subscribe. */
static void ukw_ferry_and_stash(struct wnl_fd *w, int fd, const uint8_t *reqbuf, int total)
{
	uint8_t rep[16384];
	int rl = ukw_nl_call(w->is_route ? UKW_OP_RTNL : UKW_OP_NL, reqbuf, total, rep, sizeof rep);
	/* rtnl: iproute2 accepts a reply only if its nlmsg_pid == the socket's bound
	 * port. We hand out w->fake_port via the getsockname intercept, so stamp every
	 * reply nlmsghdr's pid (offset 12) to it. genl (libnl) puts its own port in
	 * requests and the daemon echoes it, so genl needs no stamp. */
	if (rl > 0 && w->is_route && w->fake_port) {
		int o = 0;
		while (o + 16 <= rl) {
			uint32_t ml; memcpy(&ml, rep + o, 4);
			if (ml < 16 || o + (int) ml > rl) break;
			memcpy(rep + o + 12, &w->fake_port, 4);
			o += (int)((ml + 3) & ~3u);
		}
	}
	free(w->reply); w->reply = NULL; w->rlen = 0; w->roff = 0;
	if (rl > 0) { w->reply = (uint8_t *) malloc(rl); if (w->reply) { memcpy(w->reply, rep, rl); w->rlen = rl; } }
	/* auto-subscribe the triggering socket to scan events (see recvmsg event branch):
	 * nlmsghdr.nlmsg_type @off 4 (nl80211 family 0x24), genlmsghdr.cmd @off 16
	 * (NL80211_CMD_TRIGGER_SCAN 33). */
	if (!w->is_route && total >= 20) {
		uint16_t nltype; memcpy(&nltype, reqbuf + 4, 2);
		uint8_t gcmd = reqbuf[16];
		if (nltype == 0x24 && gcmd == 33) {   /* NL80211_CMD_TRIGGER_SCAN */
			w->sub = 1; w->last_gen = 0;
			ukw_dlog("auto-sub: TRIGGER_SCAN fd=%d -> sub=1\n", fd);
		}
		/* Track the REAL chip mode from nl80211 SET_INTERFACE (cmd 6) — this is what
		 * `iw dev wlan0 set type monitor` / airmon-ng use (the proven monitor path).
		 * Scan the request attrs for NL80211_ATTR_IFTYPE (5): MONITOR(6)/STATION(2).
		 * SIOCGIFHWADDR then reports ARPHRD_IEEE80211_RADIOTAP iff the chip is truly
		 * in monitor mode (set via iw), not a WEXT lie. */
		if (nltype == 0x24 && gcmd == 6 && total >= 24) {
			int o = 20;   /* after nlmsghdr(16) + genlmsghdr(4) */
			while (o + 4 <= total) {
				uint16_t alen, atype;
				memcpy(&alen, reqbuf + o, 2); memcpy(&atype, reqbuf + o + 2, 2);
				if (alen < 4 || o + (int) alen > total) break;
				if ((atype & 0x3fff) == 5 /* NL80211_ATTR_IFTYPE */ && alen >= 8) {
					uint32_t it; memcpy(&it, reqbuf + o + 4, 4);
					g_wext_mode = (it == 6) ? 6 : 2;
					ukw_dlog("nl80211 SET_INTERFACE iftype=%u -> mode=%u\n", it, g_wext_mode);
				}
				o += ((int) alen + 3) & ~3;
			}
		}
	}
	ukw_dlog("ferry fd=%d %s req=%d -> reply=%d\n", fd, w->is_route ? "rtnl" : "genl", total, rl);
}

static bool uknl_wifi_dispatch(Tracee *tracee, word_t nr)
{
	if (!uk_wifi_on()) return false;

	/* socket(AF_PACKET): Android/SELinux blocks AF_PACKET for app uids, so rewrite
	 * it to a harmless AF_UNIX SOCK_DGRAM (keeping CLOEXEC/NONBLOCK flags) — the
	 * guest gets a valid fd that we mark (at exit) and whose I/O we ferry. */
	if (nr == PR_socket) {
		int dom = (int) peek_reg(tracee, CURRENT, SYSARG_1);
		if (dom == 17 /* AF_PACKET */) {
			int t = (int) peek_reg(tracee, CURRENT, SYSARG_2);
			poke_reg(tracee, SYSARG_1, 1 /* AF_UNIX */);
			poke_reg(tracee, SYSARG_2, (word_t)((t & ~0xff) | 2 /* SOCK_DGRAM */));
			poke_reg(tracee, SYSARG_3, 0);
		} else if (dom == 16 /* AF_NETLINK */ &&
		           (int) peek_reg(tracee, CURRENT, SYSARG_3) == 16 /* NETLINK_GENERIC */) {
			/* Android/SELinux denies NETLINK_GENERIC for app uids (proven: EACCES),
			 * so create a real NETLINK_ROUTE socket instead (allowed) — bind/
			 * getsockname then work natively for libnl. It's marked is_genl at exit
			 * (from ORIGINAL proto=16), and its genl traffic (GETFAMILY, nl80211) is
			 * ferried to the daemon (UK_OP_NL); the route socket is never used for
			 * real netlink I/O. */
			poke_reg(tracee, SYSARG_3, 0 /* NETLINK_ROUTE */);
		}
		return false;   /* let it run; marked at exit from ORIGINAL args */
	}
	/* bind() on a marked AF_PACKET (now AF_UNIX dummy) socket: fake success (the
	 * guest binds a sockaddr_ll the dummy can't accept; the iface is virtual). */
	if (nr == PR_bind) {
		struct wnl_fd *w = wnl_get(ukfs_tgid(tracee->pid), (int) peek_reg(tracee, CURRENT, SYSARG_1), 0);
		if (w && w->is_packet) UKW_RET(0);
		return false;
	}
	/* getsockname() on a marked rtnl socket: return a deterministic sockaddr_nl with
	 * our fake local port. iproute2 reads its bound port here and then only accepts
	 * replies whose nlmsg_pid == that port; we stamp every rtnl reply with the same
	 * w->fake_port (see ukw_ferry_and_stash), so they always match. (genl is left to
	 * the real getsockname on the placeholder socket — libnl already works there.) */
	if (nr == PR_getsockname) {
		int fd = (int) peek_reg(tracee, CURRENT, SYSARG_1);
		struct wnl_fd *w = wnl_get(ukfs_tgid(tracee->pid), fd, 0);
		if (!w || !w->is_route || !w->fake_port) return false;
		word_t aa = peek_reg(tracee, CURRENT, SYSARG_2);
		word_t la = peek_reg(tracee, CURRENT, SYSARG_3);
		struct uk_snl_g { uint16_t nl_family, nl_pad; uint32_t nl_pid, nl_groups; } snl;
		memset(&snl, 0, sizeof snl);
		snl.nl_family = 16 /* AF_NETLINK */;
		snl.nl_pid = w->fake_port;
		socklen_t want = (socklen_t) sizeof snl, have = want;
		if (la) read_data(tracee, &have, la, sizeof have);
		socklen_t cp = have < want ? have : want;
		if (aa && cp) write_data(tracee, aa, &snl, cp);
		if (la) write_data(tracee, la, &want, sizeof want);
		UKW_RET(0);
	}

	/* SIOCGIF* ioctls on our virtual wlanN (airodump/aircrack/ifconfig query the
	 * MAC, index, flags, MTU via these on an AF_INET/AF_PACKET socket). Android
	 * denies them on a real socket (no host wlan0), so answer from the daemon.
	 * Keyed on the ifr_name (wlan*), so real interfaces fall through untouched. */
	if (nr == PR_ioctl) {
		word_t cmd = peek_reg(tracee, CURRENT, SYSARG_2);
		switch (cmd) {
		case 0x8927: case 0x8933: case 0x8913: case 0x8914:
		case 0x8921: case 0x8915: case 0x891b: case 0x8924:   /* SIOCGIF* */
		case 0x8B01: case 0x8B04: case 0x8B05: case 0x8B06: case 0x8B07:   /* WEXT */
			break;
		default: return false;   /* not an ioctl we handle */
		}
		word_t arg = peek_reg(tracee, CURRENT, SYSARG_3);
		if (!arg) return false;
		char name[16];
		if (read_data(tracee, name, arg, sizeof name) < 0) return false;
		name[15] = '\0';
		if (strncmp(name, "wlan", 4) != 0) return false;   /* not our virtual iface */
		switch (cmd) {
		case 0x8933: {   /* SIOCGIFINDEX */
			int idx = 3;   /* wlan0 (and wlan0mon) -> ifindex 3, single chip */
			write_data(tracee, arg + 16, &idx, sizeof idx);
			UKW_RET(0);
		}
		case 0x8927: {   /* SIOCGIFHWADDR -> ARP type + chip MAC */
			uint8_t mac[6]; ukw_iface_mac(mac);
			/* aircrack reads ifr_hwaddr.sa_family as the ARP link type and rejects a
			 * monitor iface that isn't ARPHRD_IEEE80211_*. We deliver radiotap frames,
			 * so report ARPHRD_IEEE80211_RADIOTAP (803) in monitor mode, else ETHER. */
			uint16_t fam = (g_wext_mode == 6) ? 803 : 1;
			uint8_t sa[8]; memcpy(sa, &fam, 2); memcpy(sa + 2, mac, 6);
			write_data(tracee, arg + 16, sa, sizeof sa);
			ukw_dlog("ioctl SIOCGIFHWADDR %s -> arp=%u %02x:%02x:%02x:%02x:%02x:%02x\n", name, fam, mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
			UKW_RET(0);
		}
		case 0x8913: {   /* SIOCGIFFLAGS: UP|BROADCAST|RUNNING|MULTICAST */
			short flags = 0x1 | 0x2 | 0x40 | 0x1000;
			write_data(tracee, arg + 16, &flags, sizeof flags);
			UKW_RET(0);
		}
		case 0x8914:     /* SIOCSIFFLAGS (promisc/up): accept (chip monitor set via iw) */
			UKW_RET(0);
		case 0x8921: {   /* SIOCGIFMTU */
			int mtu = 1500; write_data(tracee, arg + 16, &mtu, sizeof mtu); UKW_RET(0);
		}
		case 0x8915: case 0x891b: {   /* SIOCGIFADDR / SIOCGIFNETMASK: sockaddr_in, 0 addr */
			uint8_t sin[8] = { 2, 0, 0, 0, 0, 0, 0, 0 };   /* AF_INET, 0.0.0.0 */
			write_data(tracee, arg + 16, sin, sizeof sin); UKW_RET(0);
		}
		case 0x8924:     /* SIOCSIFHWADDR (macchanger): accept */
			UKW_RET(0);
		/* ---- WEXT (wireless extensions) — aircrack/iwconfig monitor path ---- */
		case 0x8B01: {   /* SIOCGIWNAME -> wireless protocol name (recognises it as wifi) */
			char wn[16]; memset(wn, 0, sizeof wn); memcpy(wn, "IEEE 802.11", 11);
			write_data(tracee, arg + 16, wn, sizeof wn);
			UKW_RET(0);
		}
		case 0x8B06: {   /* SIOCSIWMODE: set mode (u.mode @off16). 6=monitor, 2=managed. */
			uint32_t mode = 0; read_data(tracee, &mode, arg + 16, sizeof mode);
			g_wext_mode = mode;
			ukw_tx(20 /* UK_OP_SET_MONITOR */, (0u << 1) | (mode == 6 ? 1u : 0u), NULL, 0);
			ukw_dlog("ioctl SIOCSIWMODE %s -> mode=%u\n", name, mode);
			UKW_RET(0);
		}
		case 0x8B07: {   /* SIOCGIWMODE -> current mode */
			uint32_t mode = g_wext_mode ? g_wext_mode : 2u;
			write_data(tracee, arg + 16, &mode, sizeof mode);
			UKW_RET(0);
		}
		case 0x8B04: {   /* SIOCSIWFREQ: set channel/freq (struct iw_freq @off16) */
			int32_t m = 0; int16_t e = 0;
			read_data(tracee, &m, arg + 16, sizeof m);
			read_data(tracee, &e, arg + 20, sizeof e);
			int freq;
			if (e == 0) {   /* m is a channel number */
				freq = (m == 14) ? 2484 : (m <= 14 ? 2407 + m * 5 : 5000 + m * 5);
			} else {        /* m * 10^e Hz -> MHz */
				freq = m; while (e > 6) { freq *= 10; e--; } while (e < 6) { freq /= 10; e++; }
			}
			ukw_tx(23 /* UK_OP_SET_CHANNEL */, (0u << 16) | (uint32_t) freq, NULL, 0);
			ukw_dlog("ioctl SIOCSIWFREQ %s -> %d MHz\n", name, freq);
			UKW_RET(0);
		}
		case 0x8B05:     /* SIOCGIWFREQ: best-effort no-op (airodump tolerates) */
			UKW_RET(0);
		}
		return false;
	}

	/* ---- genl netlink ferry: sendmsg/recvmsg/close on a marked nl80211 fd ---- */
	if (nr == PR_close) {
		struct wnl_fd *w = wnl_get(ukfs_tgid(tracee->pid), (int) peek_reg(tracee, CURRENT, SYSARG_1), 0);
		if (w) wnl_free(w);
		return false;   /* let the real close run on the placeholder netlink fd */
	}
	/* AF_PACKET via send()/recv() == sendto/recvfrom (wpa's l2_packet uses these,
	 * NOT sendmsg). Buffer is a flat (buf,len), not an iov. */
	if (nr == PR_sendto || nr == PR_recvfrom) {
		int fd = (int) peek_reg(tracee, CURRENT, SYSARG_1);
		struct wnl_fd *w = wnl_get(ukfs_tgid(tracee->pid), fd, 0);
		if (!w) return false;
		word_t buf = peek_reg(tracee, CURRENT, SYSARG_2);
		int len = (int) peek_reg(tracee, CURRENT, SYSARG_3);
		/* netlink (rtnl/genl) over sendto/recvfrom: iproute2 sends dump requests with
		 * sendto() (NOT sendmsg) and may read with recvfrom(); ferry them like the
		 * sendmsg/recvmsg path so the daemon sees the request and the reply is stashed.
		 * Without this `ip link` (a GETLINK dump via sendto) never reached the daemon
		 * and iproute2 spun on an empty recvmsg. */
		if (!w->is_packet) {
			if (nr == PR_sendto) {
				uint8_t reqbuf[16384]; int total = len > (int) sizeof reqbuf ? (int) sizeof reqbuf : len;
				if (total > 0) read_data(tracee, reqbuf, buf, (word_t) total);
				ukw_ferry_and_stash(w, fd, reqbuf, total);
				UKW_RET(len);
			}
			/* recvfrom: deliver ONE stashed netlink message into the flat buffer. */
			int rflags = (int) peek_reg(tracee, CURRENT, SYSARG_4);
			int rpeek = (rflags & 2 /* MSG_PEEK */) != 0;
			if (w->reply && w->roff < w->rlen) {
				int avail = w->rlen - w->roff, mlen = avail;
				if (avail >= 4) { uint32_t l; memcpy(&l, w->reply + w->roff, 4); if (l >= 16 && (int) l <= avail) mlen = (int) l; }
				int n = len < mlen ? len : mlen;
				if (n > 0 && write_data(tracee, buf, w->reply + w->roff, (word_t) n) < 0) n = 0;
				if (!rpeek) { int adv = (mlen + 3) & ~3; w->roff += (adv <= avail ? adv : avail); }
				ukw_dlog("recvfrom fd=%d %s peek=%d len=%d mlen=%d roff=%d/%d\n", fd, w->is_route ? "rtnl" : "genl", rpeek, len, mlen, w->roff, w->rlen);
				UKW_RET(rpeek ? mlen : n);
			}
			UKW_RET(-EAGAIN);
		}
		if (nr == PR_sendto) {
			uint8_t fr[4096]; if (len > (int) sizeof fr) len = (int) sizeof fr;
			if (len > 0) read_data(tracee, fr, buf, (word_t) len);
			ukw_tx(w->pkt_eapol ? UKW_OP_EAPOL_TX : UKW_OP_INJECT, 0, fr, len);
			UKW_RET(len);
		}
		/* recvfrom: deliver ONE captured frame (monitor batch is split per-frame). */
		UKW_RET(ukw_packet_recv_one(tracee, w, buf, len));
	}
	/* read() on a marked AF_PACKET socket: aircrack/airodump capture with read(),
	 * not recvfrom — deliver one captured frame the same way. (read isn't trapped by
	 * default; the wifi seccomp adds it under UK_WIFI.) Non-packet reads fall through. */
	if (nr == PR_read) {
		struct wnl_fd *w = wnl_get(ukfs_tgid(tracee->pid), (int) peek_reg(tracee, CURRENT, SYSARG_1), 0);
		if (!w || !w->is_packet) return false;
		word_t buf = peek_reg(tracee, CURRENT, SYSARG_2);
		int count = (int) peek_reg(tracee, CURRENT, SYSARG_3);
		UKW_RET(ukw_packet_recv_one(tracee, w, buf, count));
	}
	if (nr == PR_sendmsg || nr == PR_recvmsg) {
		int fd = (int) peek_reg(tracee, CURRENT, SYSARG_1);
		struct wnl_fd *w = wnl_get(ukfs_tgid(tracee->pid), fd, 0);
		if (!w) return false;   /* not a marked nl80211 socket -> normal handling */
		struct msghdr mh; memset(&mh, 0, sizeof mh);
		if (read_data(tracee, &mh, peek_reg(tracee, CURRENT, SYSARG_2), sizeof mh) < 0) return false;
		unsigned long niov = (unsigned long) mh.msg_iovlen;
		if (niov > 8) niov = 8;
		struct iovec iov[8];
		if (niov && read_data(tracee, iov, (word_t)(uintptr_t) mh.msg_iov, niov * sizeof(struct iovec)) < 0) return false;

		/* ---- AF_PACKET (EAPOL 4-way handshake / monitor inject+RX) ---- */
		if (w->is_packet) {
			if (nr == PR_sendmsg) {
				uint8_t fr[4096]; int total = 0;
				for (unsigned long i = 0; i < niov; i++) {
					int l = (int) iov[i].iov_len;
					if (l <= 0 || total + l > (int) sizeof fr) break;
					if (read_data(tracee, fr + total, (word_t)(uintptr_t) iov[i].iov_base, (word_t) l) < 0) break;
					total += l;
				}
				ukw_tx(w->pkt_eapol ? UKW_OP_EAPOL_TX : UKW_OP_INJECT, 0, fr, total);
				UKW_RET(total);
			}
			/* recvmsg: deliver ONE captured frame into the first iov (monitor batch
			 * is split per-frame by the helper; aircrack uses a single iov). */
			if (niov == 0) UKW_RET(-EAGAIN);
			int copied = ukw_packet_recv_one(tracee, w, (word_t)(uintptr_t) iov[0].iov_base, (int) iov[0].iov_len);
			if (copied < 0) UKW_RET(copied);
			mh.msg_namelen = 0; mh.msg_controllen = 0; mh.msg_flags = 0;
			write_data(tracee, peek_reg(tracee, CURRENT, SYSARG_2), &mh, sizeof mh);
			UKW_RET(copied);
		}

		if (nr == PR_sendmsg) {
			uint8_t reqbuf[16384]; int total = 0;
			for (unsigned long i = 0; i < niov; i++) {
				int l = (int) iov[i].iov_len;
				if (l <= 0 || total + l > (int) sizeof reqbuf) break;
				if (read_data(tracee, reqbuf + total, (word_t)(uintptr_t) iov[i].iov_base, (word_t) l) < 0) break;
				total += l;
			}
			ukw_ferry_and_stash(w, fd, reqbuf, total);
			UKW_RET(total);   /* report all bytes "sent" */
		}
		/* recvmsg: first drain a stashed command reply (W3b-2); else, on a
		 * subscribed event socket, deliver one async event (NEW_SCAN_RESULTS).
		 * MSG_PEEK (libnl sizes the reply with a peek before the real read): copy
		 * but DON'T consume, else the real read finds the stash empty and libnl
		 * fails (this is what made GETFAMILY resolve to "nl80211 not found"). */
		int recv_flags = (int) peek_reg(tracee, CURRENT, SYSARG_3);
		int peek = (recv_flags & 2 /* MSG_PEEK */) != 0;
		int copied = 0;     /* bytes actually written to the caller's iov */
		int msglen = -1;    /* full length of the delivered message (>=0 if one is available) */
		/* If nothing is stashed and the socket is subscribed, fetch the next async
		 * event (NEW_SCAN_RESULTS/MLME) INTO the stash, so the peek-aware delivery
		 * below consumes it EXACTLY ONCE. (Delivering directly here was a bug: libnl
		 * does MSG_PEEK then a real read; the peek fetched the event and advanced
		 * last_gen, so the real read got nothing -> the event was lost and iw spun
		 * forever in recvmsg.) */
		if (!(w->reply && w->roff < w->rlen) && w->sub) {
			uint8_t ev[2048]; unsigned cur = w->last_gen;
			int el = ukw_event(w->last_gen, &cur, ev, sizeof ev);
			ukw_dlog("event fd=%d sub=1 last_gen=%u -> cur=%u el=%d\n", fd, w->last_gen, cur, el);
			w->last_gen = cur;
			if (el > 0) {
				free(w->reply);
				w->reply = (uint8_t *) malloc(el);
				if (w->reply) { memcpy(w->reply, ev, el); w->rlen = el; w->roff = 0; }
			}
		}
		if (w->reply && w->roff < w->rlen) {
			/* Deliver exactly ONE netlink message per recvmsg, mirroring the
			 * kernel's datagram semantics. libnl/iproute2 stop parsing a datagram
			 * after the first non-multipart message and issue a fresh recvmsg for
			 * the ACK; concatenating NEWFAMILY+ACK in one recvmsg loses the ACK.
			 * Copy a single nlmsg (length from its nlmsghdr) and NLMSG_ALIGN-advance. */
			int avail = w->rlen - w->roff;
			int mlen = avail;
			if (avail >= 4) {
				uint32_t l; memcpy(&l, w->reply + w->roff, 4);
				if (l >= 16 && (int) l <= avail) mlen = (int) l;
			}
			int off = 0;
			for (unsigned long i = 0; i < niov && off < mlen; i++) {
				int want = (int) iov[i].iov_len, rem = mlen - off;
				int n = want < rem ? want : rem;
				if (n <= 0) continue;
				if (write_data(tracee, (word_t)(uintptr_t) iov[i].iov_base, w->reply + w->roff + off, (word_t) n) < 0) break;
				off += n;
			}
			copied = off; msglen = mlen;
			if (!peek) {                       /* peek must not consume */
				int adv = (mlen + 3) & ~3;     /* NLMSG_ALIGN to the next message */
				w->roff += (adv <= avail ? adv : avail);
			}
		}
		ukw_dlog("recvmsg fd=%d %s peek=%d niov=%lu iov0=%d msglen=%d copied=%d sub=%d roff=%d/%d\n",
		         fd, w->is_route ? "rtnl" : "genl", peek, niov,
		         niov ? (int) iov[0].iov_len : -1, msglen, copied, w->sub, w->roff, w->rlen);
		if (msglen < 0) UKW_RET(-EAGAIN);   /* nothing pending */
		/* MSG_PEEK|MSG_TRUNC: the caller (iproute2 peeks with iov_len=0) sizes its
		 * buffer from the return value, which must be the FULL message length even
		 * if less (or nothing) was copied. Report msglen on peek; copied otherwise. */
		int retlen = peek ? msglen : copied;
		/* The message is from the "kernel": libnl's nl_recv REQUIRES the source
		 * address to be a full sockaddr_nl (msg_namelen == sizeof) with nl_family
		 * AF_NETLINK, else it returns -NLE_NOADDR and drops the reply (this is why
		 * GETFAMILY resolved to "nl80211 not found"). Write a kernel sockaddr_nl
		 * (nl_pid=0, nl_groups=0) into the caller's msg_name buffer. */
		struct uk_snl { uint16_t nl_family; uint16_t nl_pad; uint32_t nl_pid; uint32_t nl_groups; };
		if (mh.msg_name && mh.msg_namelen >= (socklen_t) sizeof(struct uk_snl)) {
			struct uk_snl snl; memset(&snl, 0, sizeof snl);
			snl.nl_family = 16 /* AF_NETLINK */;
			write_data(tracee, (word_t)(uintptr_t) mh.msg_name, &snl, sizeof snl);
			mh.msg_namelen = (socklen_t) sizeof(struct uk_snl);
		} else {
			mh.msg_namelen = 0;
		}
		mh.msg_controllen = 0; mh.msg_flags = 0;
		/* MSG_TRUNC: if the buffer was smaller than the message, flag truncation. */
		if (copied < msglen) mh.msg_flags |= 0x20 /* MSG_TRUNC */;
		write_data(tracee, peek_reg(tracee, CURRENT, SYSARG_2), &mh, sizeof mh);
		UKW_RET(retlen);
	}

	/* setsockopt(SOL_NETLINK, NETLINK_ADD_MEMBERSHIP) on a marked fd: record the
	 * subscription so recvmsg/poll deliver async events, and fake success (the
	 * real subscribe to our fake group on the placeholder socket is meaningless).
	 * last_gen=0 so a scan that already completed (iw triggers *then* subscribes)
	 * is still delivered once. */
	if (nr == PR_setsockopt) {
		int fd = (int) peek_reg(tracee, CURRENT, SYSARG_1);
		struct wnl_fd *w = wnl_get(ukfs_tgid(tracee->pid), fd, 0);
		if (!w) return false;
		int level = (int) peek_reg(tracee, CURRENT, SYSARG_2);
		int optname = (int) peek_reg(tracee, CURRENT, SYSARG_3);
		if (level == 270 && optname == 1) { w->sub = 1; w->last_gen = 0; }
		ukw_dlog("setsockopt fd=%d level=%d opt=%d -> sub=%d\n", fd, level, optname, w->sub);
		UKW_RET(0);
	}

	/* poll/ppoll: a marked fd is readable when a stashed reply remains or (for a
	 * subscribed event socket) a newer scan completed. Engage only when the set
	 * contains a marked fd; cap the wait so the guest re-polls (events arrive). */
	if (nr == PR_poll || nr == PR_ppoll) {
		unsigned long nfds = (unsigned long) peek_reg(tracee, CURRENT, SYSARG_2);
		if (nfds == 0 || nfds > 16) return false;
		word_t fa = peek_reg(tracee, CURRENT, SYSARG_1);
		struct pollfd pf[16];
		if (read_data(tracee, pf, fa, nfds * sizeof(struct pollfd)) < 0) return false;
		int pid = ukfs_tgid(tracee->pid), nwifi = 0;
		for (unsigned long i = 0; i < nfds; i++) { pf[i].revents = 0; if (wnl_get(pid, pf[i].fd, 0)) nwifi++; }
		if (nwifi == 0) return false;
		unsigned gen = ukw_scangen();
		int ready = 0;
		for (unsigned long i = 0; i < nfds; i++) {
			struct wnl_fd *w = wnl_get(pid, pf[i].fd, 0);
			if (!w) continue;
			int r;
			if (w->is_packet) {
				/* fetch a frame into the stash so readiness is accurate (and the
				 * frame isn't lost between poll and recvmsg) */
				if (!(w->reply && w->roff < w->rlen)) {
					uint8_t fr[4096];
					int fl = ukw_rx(w->pkt_eapol ? UKW_OP_EAPOL_RX : UKW_OP_MONITOR_RX, 0, fr, sizeof fr);
					if (fl > 0) { free(w->reply); w->reply = (uint8_t *) malloc(fl); if (w->reply) { memcpy(w->reply, fr, fl); w->rlen = fl; w->roff = 0; } }
				}
				r = (w->reply && w->roff < w->rlen);
			} else {
				/* subscribed genl event socket: fetch a pending event (scan or
				 * connect/MLME) into the stash so readiness is accurate */
				if (w->sub && !(w->reply && w->roff < w->rlen)) {
					uint8_t ev[2048]; unsigned cur = w->last_gen;
					int el = ukw_event(w->last_gen, &cur, ev, sizeof ev);
					w->last_gen = cur;
					if (el > 0) { free(w->reply); w->reply = (uint8_t *) malloc(el); if (w->reply) { memcpy(w->reply, ev, el); w->rlen = el; w->roff = 0; } }
				}
				r = (w->reply && w->roff < w->rlen);
			}
			(void) gen;
			if (r && (pf[i].events & POLLIN)) { pf[i].revents = POLLIN; ready++; }
		}
		if (!ready) { struct timespec ts = { 0, 50 * 1000 * 1000 }; nanosleep(&ts, NULL); }
		write_data(tracee, fa, pf, nfds * sizeof(struct pollfd));
		UKW_RET(ready);
	}

	if (nr == PR_finit_module) {
		int fd = (int) peek_reg(tracee, CURRENT, SYSARG_1);
		char path[PATH_MAX], name[64];
		if (readlink_proc_pid_fd(tracee->pid, fd, path) < 0) UKW_RET(-EINVAL);
		ukw_modname(path, name, sizeof name);
		if (!name[0]) UKW_RET(-EINVAL);
		int r = ukw_call(UKW_OP_MODPROBE, name);
		UKW_RET(r == 0 ? 0 : -EINVAL);   /* modprobe maps nonzero -> "could not insert" */
	}

	if (nr == PR_init_module) {
		/* name lives in the module's .modinfo as a "name=<mod>" string; scan the
		 * head of the image buffer for it (avoids parsing the full ELF). */
		word_t buf = peek_reg(tracee, CURRENT, SYSARG_1);
		unsigned long len = (unsigned long) peek_reg(tracee, CURRENT, SYSARG_2);
		if (len > 65536) len = 65536;
		char *blob = (char *) malloc(len + 1);
		if (!blob) UKW_RET(-ENOMEM);
		char name[64]; name[0] = '\0';
		if (len && read_data(tracee, blob, buf, (word_t) len) == 0) {
			blob[len] = '\0';
			for (unsigned long i = 0; i + 5 < len; i++)
				if (memcmp(blob + i, "name=", 5) == 0) {
					const char *v = blob + i + 5; size_t k = 0;
					while (v[k] && v[k] != '\0' && k < sizeof(name) - 1 && (unsigned char) v[k] >= 0x20) { name[k] = v[k]; k++; }
					name[k] = '\0'; break;
				}
		}
		free(blob);
		if (!name[0]) UKW_RET(-EINVAL);
		int r = ukw_call(UKW_OP_MODPROBE, name);
		UKW_RET(r == 0 ? 0 : -EINVAL);
	}

	if (nr == PR_delete_module) {
		word_t na = peek_reg(tracee, CURRENT, SYSARG_1);
		char name[64];
		if (!na || read_string(tracee, name, na, sizeof name) <= 0) UKW_RET(-EINVAL);
		int r = ukw_call(UKW_OP_RMMOD, name);
		UKW_RET(r == 0 ? 0 : -EWOULDBLOCK);   /* rmmod maps nonzero -> not loaded/busy */
	}

	return false;
}
