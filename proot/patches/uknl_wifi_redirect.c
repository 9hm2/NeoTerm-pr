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

/* wire protocol — mirrors app/src/main/cpp/ukfs/include/ukernel/proxy.h */
struct ukw_req { uint32_t op, cmd, len; };
struct ukw_rsp { int32_t ret; uint32_t len; };
#define UKW_OP_MODPROBE 30
#define UKW_OP_RMMOD    31
#define UKW_OP_NL       33

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
static int ukw_nl_call(const uint8_t *req, int reqlen, uint8_t *out, int outcap)
{
	int s = ukw_connect(); if (s < 0) return -1;
	struct ukw_req r = { UKW_OP_NL, 0, (uint32_t) reqlen };
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

/* Per-(tgid,genl-fd) state: a guest nl80211/genl netlink socket whose sendmsg is
 * ferried to the daemon (UK_OP_NL) and whose reply is drained by recvmsg. */
struct wnl_fd { int used, pid, fd; uint8_t *reply; int rlen, roff; };
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
	if (domain != 16 /* AF_NETLINK */ || proto != 16 /* NETLINK_GENERIC */) return;
	int fd = (int) peek_reg(tracee, CURRENT, SYSARG_RESULT);
	if (fd < 0) return;
	wnl_get(ukfs_tgid(tracee->pid), fd, 1);
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

	/* ---- genl netlink ferry: sendmsg/recvmsg/close on a marked nl80211 fd ---- */
	if (nr == PR_close) {
		struct wnl_fd *w = wnl_get(ukfs_tgid(tracee->pid), (int) peek_reg(tracee, CURRENT, SYSARG_1), 0);
		if (w) wnl_free(w);
		return false;   /* let the real close run on the placeholder netlink fd */
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

		if (nr == PR_sendmsg) {
			uint8_t reqbuf[16384]; int total = 0;
			for (unsigned long i = 0; i < niov; i++) {
				int l = (int) iov[i].iov_len;
				if (l <= 0 || total + l > (int) sizeof reqbuf) break;
				if (read_data(tracee, reqbuf + total, (word_t)(uintptr_t) iov[i].iov_base, (word_t) l) < 0) break;
				total += l;
			}
			uint8_t rep[16384];
			int rl = ukw_nl_call(reqbuf, total, rep, sizeof rep);
			free(w->reply); w->reply = NULL; w->rlen = 0; w->roff = 0;
			if (rl > 0) { w->reply = (uint8_t *) malloc(rl); if (w->reply) { memcpy(w->reply, rep, rl); w->rlen = rl; } }
			UKW_RET(total);   /* report all bytes "sent" */
		}
		/* recvmsg: drain the stashed reply into the iov buffers */
		if (!w->reply || w->roff >= w->rlen) UKW_RET(-EAGAIN);
		int copied = 0;
		for (unsigned long i = 0; i < niov && w->roff < w->rlen; i++) {
			int want = (int) iov[i].iov_len, avail = w->rlen - w->roff;
			int n = want < avail ? want : avail;
			if (n <= 0) continue;
			if (write_data(tracee, (word_t)(uintptr_t) iov[i].iov_base, w->reply + w->roff, (word_t) n) < 0) break;
			w->roff += n; copied += n;
		}
		/* a netlink dgram socket reports no src address / no control data */
		mh.msg_namelen = 0; mh.msg_controllen = 0; mh.msg_flags = 0;
		write_data(tracee, peek_reg(tracee, CURRENT, SYSARG_2), &mh, sizeof mh);
		UKW_RET(copied);
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
