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

/* Per-(tgid,genl-fd) state: a guest nl80211/genl netlink socket whose sendmsg is
 * ferried to the daemon (UK_OP_NL) and whose reply is drained by recvmsg. */
struct wnl_fd { int used, pid, fd; uint8_t *reply; int rlen, roff; int sub; unsigned last_gen;
                int is_packet, pkt_eapol;   /* AF_PACKET: EAPOL (0x888e) vs monitor (ETH_P_ALL) */
                int is_route; };            /* AF_NETLINK NETLINK_ROUTE (ip link/addr) */
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
		if (w) w->is_route = 1;
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

	/* ---- genl netlink ferry: sendmsg/recvmsg/close on a marked nl80211 fd ---- */
	if (nr == PR_close) {
		struct wnl_fd *w = wnl_get(ukfs_tgid(tracee->pid), (int) peek_reg(tracee, CURRENT, SYSARG_1), 0);
		if (w) wnl_free(w);
		return false;   /* let the real close run on the placeholder netlink fd */
	}
	/* AF_PACKET via send()/recv() == sendto/recvfrom (wpa's l2_packet uses these,
	 * NOT sendmsg). Buffer is a flat (buf,len), not an iov. */
	if (nr == PR_sendto || nr == PR_recvfrom) {
		struct wnl_fd *w = wnl_get(ukfs_tgid(tracee->pid), (int) peek_reg(tracee, CURRENT, SYSARG_1), 0);
		if (!w || !w->is_packet) return false;
		word_t buf = peek_reg(tracee, CURRENT, SYSARG_2);
		int len = (int) peek_reg(tracee, CURRENT, SYSARG_3);
		if (nr == PR_sendto) {
			uint8_t fr[4096]; if (len > (int) sizeof fr) len = (int) sizeof fr;
			if (len > 0) read_data(tracee, fr, buf, (word_t) len);
			ukw_tx(w->pkt_eapol ? UKW_OP_EAPOL_TX : UKW_OP_INJECT, 0, fr, len);
			UKW_RET(len);
		}
		/* recvfrom: stash-drain or fetch one frame */
		uint8_t fr[4096]; int fl = 0; uint8_t *src = NULL; int copied = 0;
		if (w->reply && w->roff < w->rlen) { src = w->reply + w->roff; fl = w->rlen - w->roff; }
		else { fl = ukw_rx(w->pkt_eapol ? UKW_OP_EAPOL_RX : UKW_OP_MONITOR_RX, 0, fr, sizeof fr); src = fr; }
		if (fl > 0) {
			int n = len < fl ? len : fl;
			if (write_data(tracee, buf, src, (word_t) n) == 0) { copied = n; if (w->reply && w->roff < w->rlen) w->roff += n; }
		}
		if (copied == 0) UKW_RET(-EAGAIN);
		UKW_RET(copied);
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
			/* recvmsg: drain a poll-stashed frame, else fetch one now */
			int copied = 0; uint8_t fr[4096]; int fl = 0; uint8_t *src = NULL;
			if (w->reply && w->roff < w->rlen) { src = w->reply + w->roff; fl = w->rlen - w->roff; }
			else { fl = ukw_rx(w->pkt_eapol ? UKW_OP_EAPOL_RX : UKW_OP_MONITOR_RX, 0, fr, sizeof fr); src = fr; }
			for (unsigned long i = 0; i < niov && copied < fl; i++) {
				int want = (int) iov[i].iov_len, avail = fl - copied;
				int n = want < avail ? want : avail;
				if (n <= 0) continue;
				if (write_data(tracee, (word_t)(uintptr_t) iov[i].iov_base, src + copied, (word_t) n) < 0) break;
				copied += n;
			}
			if (w->reply && w->roff < w->rlen) { w->roff += copied; }   /* consumed from stash */
			if (copied == 0) UKW_RET(-EAGAIN);
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
			uint8_t rep[16384];
			int rl = ukw_nl_call(w->is_route ? UKW_OP_RTNL : UKW_OP_NL, reqbuf, total, rep, sizeof rep);
			/* iproute2 (rtnl) sends requests with nlmsg_pid=0 and ACCEPTS a reply only
			 * if its nlmsg_pid equals the socket's own bound port; the daemon echoes
			 * the request pid (0), so iproute2 skips every reply and busy-loops.
			 * Rewrite each reply nlmsghdr's nlmsg_pid (offset 12) to the socket's real
			 * local port (getsockname). ONLY for rtnl: libnl (genl) already puts its
			 * own port in requests and the daemon echoes it, so genl needs no stamp —
			 * and stamping it regressed the genl path. */
			if (rl > 0 && w->is_route) {
				struct uk_snl_l { uint16_t f, pad; uint32_t pid, grp; } la;
				socklen_t ll = sizeof la;
				if (getsockname(fd, (void *) &la, &ll) == 0 && la.pid != 0) {
					int o = 0;
					while (o + 16 <= rl) {
						uint32_t ml; memcpy(&ml, rep + o, 4);
						if (ml < 16 || o + (int) ml > rl) break;
						memcpy(rep + o + 12, &la.pid, 4);
						o += (int)((ml + 3) & ~3u);
					}
				}
			}
			free(w->reply); w->reply = NULL; w->rlen = 0; w->roff = 0;
			if (rl > 0) { w->reply = (uint8_t *) malloc(rl); if (w->reply) { memcpy(w->reply, rep, rl); w->rlen = rl; } }
			/* Auto-subscribe the triggering socket to scan events. iw normally
			 * resolves the "scan" mcast group (nl_get_multicast_id) then subscribes
			 * via setsockopt(NETLINK_ADD_MEMBERSHIP); but that nested-attr resolution
			 * is fragile across libnl versions and when it fails iw never subscribes,
			 * so NEW_SCAN_RESULTS is never delivered and `iw scan` hangs forever in
			 * recvmsg. The socket that sent TRIGGER_SCAN is exactly the one that then
			 * waits for the result, so mark it subscribed (sub=1) and reset its event
			 * generation (last_gen=0) -> the next recvmsg on it delivers the scan-done
			 * event regardless of whether the formal subscription succeeded.
			 * nlmsghdr.nlmsg_type @off 4 (==nl80211 family id 0x24), genlmsghdr.cmd
			 * @off 16 (==NL80211_CMD_TRIGGER_SCAN 33). */
			if (!w->is_route && total >= 20) {
				uint16_t nltype; memcpy(&nltype, reqbuf + 4, 2);
				if (nltype == 0x24 && reqbuf[16] == 33) {
					w->sub = 1; w->last_gen = 0;
					ukw_dlog("auto-sub: TRIGGER_SCAN fd=%d -> sub=1\n", fd);
				}
			}
			ukw_dlog("sendmsg fd=%d %s req=%d -> reply=%d\n", fd, w->is_route ? "rtnl" : "genl", total, rl);
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
		} else if (w->sub) {
			uint8_t ev[2048]; unsigned cur = w->last_gen;
			int el = ukw_event(w->last_gen, &cur, ev, sizeof ev);
			ukw_dlog("event fd=%d sub=1 last_gen=%u -> cur=%u el=%d\n", fd, w->last_gen, cur, el);
			w->last_gen = cur;
			if (el > 0) {
				int off = 0;
				for (unsigned long i = 0; i < niov && off < el; i++) {
					int want = (int) iov[i].iov_len, rem = el - off;
					int n = want < rem ? want : rem;
					if (n <= 0) continue;
					if (write_data(tracee, (word_t)(uintptr_t) iov[i].iov_base, ev + off, (word_t) n) < 0) break;
					off += n;
				}
				copied = off; msglen = el;
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
