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
