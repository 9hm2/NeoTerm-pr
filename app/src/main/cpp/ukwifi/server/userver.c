/* uKernel uServer — a kernel-emulációs réteg vezérlő processze + file/ioctl proxy.
 *
 * Életciklus:
 *   1. dlopen libkernel_shim.so (RTLD_GLOBAL) — a driver innen oldja fel a
 *      kernel-szimbólumait (printk, usb_register, kmalloc, ...).
 *   2. HCD backend kiválasztása (libusb = valós eszköz, mock = emuláció).
 *   3. dlopen a driver .so — az ELF-konstruktorok bejegyzik a module_init/exit-et.
 *   4. ukernel_run_module_inits() — driver module_init -> usb_register.
 *   5. ukernel_usb_enumerate_and_probe(vid,pid) — usb_device felépítése + probe().
 *   6. (--serve) UNIX socket proxy: a kliensek open/read/write/ioctl kérései a
 *      betöltött driver file_operations-ére -> usb I/O -> valós eszköz.
 */
#include "ukernel/runtime.h"
#include "ukernel/proxy.h"
#include "modmgr.h"
#include <linux/ioctl.h>      /* _IOC_SIZE — a mi fejlécünk */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <libgen.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <execinfo.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stddef.h>

struct shim_api {
	void   (*set_loglevel)(int);
	void   (*set_hcd)(const struct ukernel_hcd_ops *);
	const struct ukernel_hcd_ops *(*hcd_usbfs)(void);   /* libusb HELYETT: usbfs-backend (NeoTerm fd + USBDEVFS) */
	const struct ukernel_hcd_ops *(*hcd_mock)(void);
	int    (*run_inits)(void);
	void   (*run_exits)(void);
	int    (*enumerate_and_probe)(uint16_t, uint16_t);
	size_t (*cdev_count)(void);
	const char *(*cdev_name)(size_t);
	void  *(*file_open)(const char *, unsigned int);
	long   (*file_read)(void *, void *, size_t);
	long   (*file_write)(void *, const void *, size_t);
	long   (*file_ioctl)(void *, unsigned int, void *);
	unsigned int (*file_poll)(void *);
	void   (*file_close)(void *);
	/* vezeték nélküli (opcionális — csak ha cfg80211.so betöltve) */
	size_t (*wiphy_count)(void);
	const char *(*wiphy_name)(size_t);
	int    (*wiphy_scan)(size_t);
	size_t (*wiphy_bss_count)(size_t);
	int    (*wiphy_bss_get)(size_t, size_t, struct uk_bss_info *);
};
static struct shim_api A;

static void *must_sym(void *h, const char *name)
{
	void *s = dlsym(h, name);
	if (!s) { fprintf(stderr, "uServer: hiányzó szimbólum: %s\n", name); exit(2); }
	return s;
}

/* ===== socket segédek ===== */
static int read_full(int fd, void *buf, size_t n)
{
	char *p = buf; size_t got = 0;
	while (got < n) { ssize_t r = read(fd, p + got, n - got); if (r <= 0) return -1; got += r; }
	return 0;
}
static int write_full(int fd, const void *buf, size_t n)
{
	const char *p = buf; size_t put = 0;
	while (put < n) { ssize_t r = write(fd, p + put, n - put); if (r <= 0) return -1; put += r; }
	return 0;
}
static int send_rsp(int fd, int32_t ret, const void *payload, uint32_t len)
{
	struct uk_rsp rsp = { ret, len };
	if (write_full(fd, &rsp, sizeof(rsp))) return -1;
	if (len && write_full(fd, payload, len)) return -1;
	return 0;
}

/* ===== egy kliens-kapcsolat kiszolgálása ===== */
static void *conn_thread(void *arg)
{
	int fd = (int)(intptr_t)arg;
	void *fh = NULL;
	char payload[4096];

	for (;;) {
		struct uk_req req;
		if (read_full(fd, &req, sizeof(req))) break;
		uint32_t len = req.len > sizeof(payload) ? sizeof(payload) : req.len;
		if (len && read_full(fd, payload, len)) break;
		/* ha a kérés > payload-puffer: a maradékot is leolvassuk (elnyeljük), különben
		 * a stream szétcsúszik (a maradék a következő req-ként értelmeződne) */
		if (req.len > len) {
			uint32_t rest = req.len - len; char drain[1024];
			while (rest) { uint32_t c = rest > sizeof(drain) ? sizeof(drain) : rest;
				if (read_full(fd, drain, c)) break; rest -= c; }
			if (rest) break;
		}

		/* === modul-kezelés: modprobe / rmmod / lsmod === */
		if (req.op == UK_OP_MODPROBE) {
			char name[64]; snprintf(name, sizeof name, "%.*s", (int)len, payload);
			send_rsp(fd, ukw_modprobe(name), NULL, 0);
			continue;
		}
		if (req.op == UK_OP_RMMOD) {
			char name[64]; snprintf(name, sizeof name, "%.*s", (int)len, payload);
			send_rsp(fd, ukw_rmmod(name), NULL, 0);
			continue;
		}
		if (req.op == UK_OP_LSMOD) {
			char list[4096]; int n = ukw_lsmod(list, sizeof list);
			send_rsp(fd, n, list, (uint32_t)(n > 0 ? n : 0));
			continue;
		}

		/* === vezeték nélküli opok (nem kell megnyitott file) === */
		if (req.op == UK_OP_LIST_WIPHY) {
			send_rsp(fd, A.wiphy_count ? (int32_t)A.wiphy_count() : -1, NULL, 0);
			continue;
		}
		if (req.op == UK_OP_SCAN) {
			int32_t r = A.wiphy_scan ? A.wiphy_scan(req.cmd) : -1;
			send_rsp(fd, r, NULL, 0);
			continue;
		}
		if (req.op == UK_OP_GET_BSS) {
			struct uk_bss_info bi;
			size_t wi = req.cmd >> 16, bidx = req.cmd & 0xffff;
			int32_t r = (A.wiphy_bss_get && A.wiphy_bss_get(wi, bidx, &bi) == 0) ? 0 : -1;
			send_rsp(fd, r, &bi, r == 0 ? sizeof(bi) : 0);
			continue;
		}
		if (req.op == UK_OP_GET_IFACE) {
			/* a VALÓDI netdev adatai (név + chip-MAC) a driverből */
			static int (*nd_info)(int, char *, unsigned char *, unsigned int *);
			if (!nd_info) nd_info = dlsym(RTLD_DEFAULT, "ukernel_netdev_info");
			struct uk_iface_info ii; memset(&ii, 0, sizeof(ii));
			unsigned int flags = 0;
			int32_t r = (nd_info && nd_info(req.cmd, ii.name, ii.mac, &flags) == 0) ? 0 : -1;
			ii.ifindex = (int32_t)req.cmd + 3;   /* netdev[0] -> ifindex 3 */
			ii.wiphy_idx = 0;
			ii.flags = flags;
			static int (*getfreq)(void);
			if (!getfreq) getfreq = dlsym(RTLD_DEFAULT, "ukernel_wiphy_get_chan_freq");
			ii.freq = getfreq ? (uint32_t)getfreq() : 0;   /* a chip aktuális csatornája */
			/* VALÓS hálózati állapot: DHCP-ből tanult cím + keret-számlálók */
			static void (*getip)(uint32_t *, uint32_t *, uint32_t *);
			static void (*getcnt)(uint64_t *, uint64_t *, uint64_t *, uint64_t *);
			if (!getip)  getip  = dlsym(RTLD_DEFAULT, "ukernel_netdev_get_ipinfo");
			if (!getcnt) getcnt = dlsym(RTLD_DEFAULT, "ukernel_netdev_get_counters");
			if (getip)  getip(&ii.ip, &ii.gw, &ii.netmask);
			if (getcnt) getcnt(&ii.tx_packets, &ii.tx_bytes, &ii.rx_packets, &ii.rx_bytes);
			static uint32_t (*getmtu)(void);
			if (!getmtu) getmtu = dlsym(RTLD_DEFAULT, "ukernel_netdev_get_mtu");
			ii.mtu = getmtu ? getmtu() : 1500;
			/* az átjáró MAC-je, ha a backend már tanult ARP-választ (a handlerek így
			 * kihagyják a per-kapcsolat ARP-ot a flaky linken) */
			static int (*getgw)(unsigned char *);
			if (!getgw) getgw = dlsym(RTLD_DEFAULT, "ukernel_netdev_get_gwmac");
			if (getgw) getgw(ii.gw_mac);
			send_rsp(fd, r, &ii, r == 0 ? sizeof(ii) : 0);
			continue;
		}
		if (req.op == UK_OP_SET_IFFLAGS) {
			static int (*setup)(int, int);
			if (!setup) setup = dlsym(RTLD_DEFAULT, "ukernel_netdev_set_up");
			int32_t r = setup ? setup((int)(req.cmd >> 1), (int)(req.cmd & 1)) : -1;
			send_rsp(fd, r, NULL, 0);
			continue;
		}
		if (req.op == UK_OP_SET_IFADDR) {
			static void (*setaddr)(uint32_t, uint32_t);
			if (!setaddr) setaddr = dlsym(RTLD_DEFAULT, "ukernel_netdev_set_addr");
			uint32_t a[2] = {0, 0}; memcpy(a, payload, len < sizeof(a) ? len : sizeof(a));
			if (setaddr) setaddr(a[0], a[1]);
			send_rsp(fd, 0, NULL, 0);
			continue;
		}
		if (req.op == UK_OP_SET_MTU) {
			static void (*setmtu)(uint32_t);
			if (!setmtu) setmtu = dlsym(RTLD_DEFAULT, "ukernel_netdev_set_mtu");
			if (setmtu) setmtu(req.cmd & 0xffff);
			send_rsp(fd, 0, NULL, 0);
			continue;
		}
		if (req.op == UK_OP_SET_MAC) {
			static int (*setmac)(int, const unsigned char *);
			if (!setmac) setmac = dlsym(RTLD_DEFAULT, "ukernel_netdev_set_mac");
			int32_t r = (setmac && len >= 6) ? setmac((int)req.cmd, (const unsigned char *)payload) : -1;
			send_rsp(fd, r, NULL, 0);
			continue;
		}
		if (req.op == UK_OP_CONNECT) {
			static int (*wconn)(size_t, const char *, int, const uint8_t *, int, const uint8_t *, int);
			static int (*wbssid)(uint8_t *);
			if (!wconn) wconn = dlsym(RTLD_DEFAULT, "ukernel_wiphy_connect");
			if (!wbssid) wbssid = dlsym(RTLD_DEFAULT, "ukernel_wiphy_conn_bssid");
			struct uk_connect_req cr; memset(&cr, 0, sizeof(cr));
			memcpy(&cr, payload, len < sizeof(cr) ? len : sizeof(cr));
			int32_t st = wconn ? wconn(req.cmd, cr.ssid, cr.ssid_len, cr.bssid, cr.freq, cr.ie, cr.ie_len) : -1;
			uint8_t bssid[6] = {0}; if (wbssid) wbssid(bssid);
			printf("uServer: CONNECT '%s' -> status=%d\n", cr.ssid, st); fflush(stdout);
			send_rsp(fd, st, bssid, 6);
			continue;
		}

		if (req.op == UK_OP_EAPOL_TX) {
			static int (*xmit)(int, const uint8_t *, int);
			if (!xmit) xmit = dlsym(RTLD_DEFAULT, "ukernel_netdev_xmit");
			int32_t r = xmit ? xmit(req.cmd, (const uint8_t *)payload, len) : -1;
			send_rsp(fd, r, NULL, 0);
			continue;
		}
		if (req.op == UK_OP_EAPOL_RX) {
			static int (*rxget)(uint8_t *, int);
			if (!rxget) rxget = dlsym(RTLD_DEFAULT, "ukernel_eapol_rx_get");
			uint8_t frame[2048]; int n = rxget ? rxget(frame, sizeof(frame)) : 0;
			send_rsp(fd, n, frame, n > 0 ? n : 0);
			continue;
		}
		if (req.op == UK_OP_DATA_TX) {
			static int (*xeth)(int, const uint8_t *, int);
			if (!xeth) xeth = dlsym(RTLD_DEFAULT, "ukernel_netdev_xmit_eth");
			int32_t r = xeth ? xeth(req.cmd, (const uint8_t *)payload, len) : -1;
			send_rsp(fd, r, NULL, 0);
			continue;
		}
		if (req.op == UK_OP_DATA_RX) {
			static int (*drxget)(uint8_t *, int);
			if (!drxget) drxget = dlsym(RTLD_DEFAULT, "ukernel_data_rx_get");
			uint8_t frame[2048]; int n = drxget ? drxget(frame, sizeof(frame)) : 0;
			send_rsp(fd, n, frame, n > 0 ? n : 0);
			continue;
		}
		if (req.op == UK_OP_ADD_KEY) {
			static int (*akey)(size_t, int, int, const uint8_t *, uint32_t, const uint8_t *, int, const uint8_t *, int);
			if (!akey) akey = dlsym(RTLD_DEFAULT, "ukernel_wiphy_add_key");
			struct uk_key_req kr; memset(&kr, 0, sizeof(kr));
			memcpy(&kr, payload, len < sizeof(kr) ? len : sizeof(kr));
			int32_t r = akey ? akey(req.cmd, kr.key_idx, kr.pairwise, kr.mac, kr.cipher,
			                        kr.key, kr.key_len, kr.seq, kr.seq_len) : -1;
			send_rsp(fd, r, NULL, 0);
			continue;
		}
		if (req.op == UK_OP_SET_MONITOR) {
			static int (*setmon)(size_t, int);
			if (!setmon) setmon = dlsym(RTLD_DEFAULT, "ukernel_wiphy_set_monitor");
			int32_t r = setmon ? setmon(req.cmd >> 1, req.cmd & 1) : -1;
			send_rsp(fd, r, NULL, 0);
			continue;
		}
		if (req.op == UK_OP_INJECT) {
			static int (*inj)(int, const uint8_t *, int);
			if (!inj) inj = dlsym(RTLD_DEFAULT, "ukernel_netdev_inject");
			int32_t r = inj ? inj(req.cmd, (const uint8_t *)payload, len) : -1;
			send_rsp(fd, r, NULL, 0);
			continue;
		}
		if (req.op == UK_OP_MONITOR_RX) {
			static int (*mrx)(uint8_t *, int);
			if (!mrx) mrx = dlsym(RTLD_DEFAULT, "ukernel_monitor_rx_get");
			static uint8_t mframe[65536]; int n = mrx ? mrx(mframe, sizeof(mframe)) : 0;  /* batch */
			send_rsp(fd, n, mframe, n > 0 ? n : 0);
			continue;
		}
		if (req.op == UK_OP_SET_CHANNEL) {
			static int (*sch)(size_t, int);
			if (!sch) sch = dlsym(RTLD_DEFAULT, "ukernel_wiphy_set_channel");
			int32_t r = sch ? sch(req.cmd >> 16, req.cmd & 0xffff) : -1;
			send_rsp(fd, r, NULL, 0);
			continue;
		}

		if (req.op == UK_OP_OPEN) {
			char name[64]; snprintf(name, sizeof(name), "%.*s", (int)len, payload);
			fh = A.file_open(name, req.cmd);
			send_rsp(fd, fh ? 0 : -1, NULL, 0);
		} else if (!fh) {
			send_rsp(fd, -1, NULL, 0);
		} else if (req.op == UK_OP_READ) {
			uint32_t want = req.cmd > sizeof(payload) ? sizeof(payload) : req.cmd;
			long r = A.file_read(fh, payload, want);
			send_rsp(fd, (int32_t)r, payload, r > 0 ? (uint32_t)r : 0);
		} else if (req.op == UK_OP_WRITE) {
			long r = A.file_write(fh, payload, len);
			send_rsp(fd, (int32_t)r, NULL, 0);
		} else if (req.op == UK_OP_IOCTL) {
			uint32_t sz = _IOC_SIZE(req.cmd);
			if (sz > sizeof(payload)) sz = sizeof(payload);
			/* a payload a bemenő arg (len bájt); a len-en túli rész stale lehet egy
			 * korábbi kérésből -> nullázzuk, hogy ne szivárogjon ki / ne adjon szemetet */
			if (sz > len) memset(payload + len, 0, sz - len);
			long r = A.file_ioctl(fh, req.cmd, payload);
			send_rsp(fd, (int32_t)r, payload, sz);
		} else if (req.op == UK_OP_POLL) {
			unsigned int m = A.file_poll(fh);
			send_rsp(fd, (int32_t)m, NULL, 0);
		} else if (req.op == UK_OP_CLOSE) {
			send_rsp(fd, 0, NULL, 0);
			break;
		} else {
			send_rsp(fd, -1, NULL, 0);
		}
	}
	if (fh) A.file_close(fh);
	close(fd);
	return NULL;
}

static volatile int g_run = 1;
static void on_sigint(int s) { (void)s; g_run = 0; }

static int proxy_serve(const char *sockpath)
{
	signal(SIGPIPE, SIG_IGN);
	/* SA_RESTART NÉLKÜL telepítjük, hogy a SIGINT/SIGTERM megszakítsa az
	 * accept()-et (különben a glibc signal() újraindítaná, és nem állna le). */
	struct sigaction sa = { 0 };
	sa.sa_handler = on_sigint;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	int srv = socket(AF_UNIX, SOCK_STREAM, 0);
	if (srv < 0) { perror("socket"); return -1; }
	struct sockaddr_un addr = { 0 };
	addr.sun_family = AF_UNIX;
	socklen_t addrlen;
	if (sockpath[0] == '@') {
		/* abstract namespace (NeoTerm convention, e.g. @io.neoterm.wifi): leading
		 * NUL, name in the rest; length must stop at the name (no trailing NULs). */
		size_t nl = strlen(sockpath + 1);
		if (nl > sizeof(addr.sun_path) - 1) nl = sizeof(addr.sun_path) - 1;
		addr.sun_path[0] = '\0';
		memcpy(addr.sun_path + 1, sockpath + 1, nl);
		addrlen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + nl);
	} else {
		snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sockpath);
		unlink(sockpath);
		addrlen = sizeof(addr);
	}
	if (bind(srv, (struct sockaddr *)&addr, addrlen) < 0) { perror("bind"); return -1; }
	if (listen(srv, 8) < 0) { perror("listen"); return -1; }

	printf("uServer: proxy figyel: %s\n", sockpath);
	printf("uServer: közzétett eszközök:");
	for (size_t i = 0; i < A.cdev_count(); i++) printf(" %s", A.cdev_name(i));
	printf("\n");

	while (g_run) {
		int c = accept(srv, NULL, NULL);
		if (c < 0) { if (g_run) perror("accept"); break; }
		pthread_t t;
		pthread_create(&t, NULL, conn_thread, (void *)(intptr_t)c);
		pthread_detach(t);
	}
	close(srv);
	unlink(sockpath);
	printf("uServer: proxy leállt\n");
	return 0;
}

static void usage(const char *p)
{
	fprintf(stderr,
		"Használat: %s [opciók] <modul1.so> [modul2.so ...]\n"
		"  A modulok a megadott sorrendben töltődnek (pl. cfg80211.so a driver előtt).\n"
		"  --hcd usbfs|mock    HCD backend (alap: usbfs = valós eszköz a NeoTerm-socketen)\n"
		"  --vid 0xVVVV        eszköz VID (alap: 0x2357)\n"
		"  --pid 0xPPPP        eszköz PID (alap: 0x011e)\n"
		"  --shim PATH         libkernel_shim.so útvonala\n"
		"  --serve             ioctl/file proxy indítása probe után\n"
		"  --sock PATH         proxy socket (alap: %s)\n"
		"  --loglevel N        0..7 (alap: 7)\n", p, UK_PROXY_SOCK_DEFAULT);
}

static void uk_segv(int sig)
{
	void *bt[64]; int n = backtrace(bt, 64);
	fprintf(stderr, "\n=== uServer CRASH (signal %d) — backtrace ===\n", sig);
	fflush(stderr);
	backtrace_symbols_fd(bt, n, 2);
	signal(sig, SIG_DFL); raise(sig);
}

int main(int argc, char **argv)
{
	signal(SIGSEGV, uk_segv); signal(SIGABRT, uk_segv);
	const char *hcd = "usbfs", *shim_path = NULL;
	const char *modules[16]; int nmodules = 0;
	const char *sockpath = UK_PROXY_SOCK_DEFAULT;
	unsigned vid = 0x2357, pid = 0x011e;
	int loglevel = 7, serve = 0, do_scan = 0, do_up = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--hcd") && i + 1 < argc) hcd = argv[++i];
		else if (!strcmp(argv[i], "--vid") && i + 1 < argc) vid = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--pid") && i + 1 < argc) pid = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--shim") && i + 1 < argc) shim_path = argv[++i];
		else if (!strcmp(argv[i], "--sock") && i + 1 < argc) sockpath = argv[++i];
		else if (!strcmp(argv[i], "--serve")) serve = 1;
		else if (!strcmp(argv[i], "--scan")) do_scan = 1;
		else if (!strcmp(argv[i], "--up")) do_up = 1;
		else if (!strcmp(argv[i], "--loglevel") && i + 1 < argc) loglevel = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
		else if (nmodules < 16) modules[nmodules++] = argv[i];
	}
	/* Chip-agnostic serve mode: a daemon may start with NO driver module — the
	 * guest's `modprobe` loads the vendor .so later (UK_OP_MODPROBE). Only the
	 * one-shot CLI modes (no --serve) require a module up front. */
	if (nmodules == 0 && !serve) { usage(argv[0]); return 1; }

	/* Two deployment models:
	 *  - separate .so (dev/host): --shim libkernel_shim.so → dlopen it.
	 *  - single binary (NeoTerm libukwifid.so): the shim + cfg80211 are statically
	 *    linked into this image, so resolve the symbols from our OWN global scope
	 *    via dlopen(NULL). (The image is linked with -Wl,--export-dynamic so its
	 *    symbols are in the dynamic table for dlsym and for a dlopen'd driver.) */
	void *sh;
	if (shim_path) {
		printf("uServer: shim betöltése: %s\n", shim_path);
		sh = dlopen(shim_path, RTLD_NOW | RTLD_GLOBAL);
		if (!sh) { fprintf(stderr, "uServer: shim dlopen hiba: %s\n", dlerror()); return 2; }
	} else {
		printf("uServer: beépített shim (single-binary)\n");
		sh = dlopen(NULL, RTLD_NOW | RTLD_GLOBAL);
		if (!sh) { fprintf(stderr, "uServer: self dlopen hiba: %s\n", dlerror()); return 2; }
	}

	A.set_loglevel = must_sym(sh, "ukernel_set_loglevel");
	A.set_hcd      = must_sym(sh, "ukernel_set_hcd");
	A.hcd_usbfs    = must_sym(sh, "ukernel_hcd_usbfs");
	A.hcd_mock     = must_sym(sh, "ukernel_hcd_mock");
	A.run_inits    = must_sym(sh, "ukernel_run_module_inits");
	A.run_exits    = must_sym(sh, "ukernel_run_module_exits");
	A.enumerate_and_probe = must_sym(sh, "ukernel_usb_enumerate_and_probe");
	A.cdev_count   = must_sym(sh, "ukernel_cdev_count");
	A.cdev_name    = must_sym(sh, "ukernel_cdev_name");
	A.file_open    = must_sym(sh, "ukernel_file_open");
	A.file_read    = must_sym(sh, "ukernel_file_read");
	A.file_write   = must_sym(sh, "ukernel_file_write");
	A.file_ioctl   = must_sym(sh, "ukernel_file_ioctl");
	A.file_poll    = must_sym(sh, "ukernel_file_poll");
	A.file_close   = must_sym(sh, "ukernel_file_close");

	/* module manager: `modprobe`/`rmmod`/`lsmod` from the guest load/unload the
	 * pre-built vendor driver .so. Modules live next to the daemon by default
	 * (the app's lib dir), overridable via $UK_WIFI_MODDIR. */
	char argv0[1024]; snprintf(argv0, sizeof argv0, "%s", argv[0]);
	struct ukw_mod_ops mops = { A.run_inits, A.run_exits, A.enumerate_and_probe };
	ukw_modmgr_init(dirname(argv0), &mops, (uint16_t)vid, (uint16_t)pid);

	A.set_loglevel(loglevel);
	const struct ukernel_hcd_ops *ops = strcmp(hcd, "mock") == 0 ? A.hcd_mock() : A.hcd_usbfs();
	printf("uServer: HCD backend = %s\n", ops->name);
	A.set_hcd(ops);

	/* modulok betöltése a megadott SORRENDBEN (pl. cfg80211.so a driver előtt).
	 * Mind RTLD_GLOBAL, hogy a későbbi modulok a korábbiak szimbólumait lássák. */
	for (int i = 0; i < nmodules; i++) {
		printf("uServer: modul betöltése [%d/%d]: %s\n", i + 1, nmodules, modules[i]);
		void *mh = dlopen(modules[i], RTLD_NOW | RTLD_GLOBAL);
		if (!mh) { fprintf(stderr, "uServer: modul dlopen hiba: %s\n", dlerror()); return 3; }
		/* register for lsmod (basename without dir / .so suffix) */
		char nb[128]; snprintf(nb, sizeof nb, "%s", basename(modules[i]));
		char *dot = strstr(nb, ".so"); if (dot) *dot = 0;
		ukw_modmgr_note(nb, mh, 0);
	}

	/* vezeték nélküli API felderítése (ha valamelyik modul — pl. cfg80211.so —
	 * exportálja). RTLD_DEFAULT az összes betöltött lib globális szkópjában keres. */
	A.wiphy_count     = dlsym(RTLD_DEFAULT, "ukernel_wiphy_count");
	A.wiphy_name      = dlsym(RTLD_DEFAULT, "ukernel_wiphy_name");
	A.wiphy_scan      = dlsym(RTLD_DEFAULT, "ukernel_wiphy_scan");
	A.wiphy_bss_count = dlsym(RTLD_DEFAULT, "ukernel_wiphy_bss_count");
	A.wiphy_bss_get   = dlsym(RTLD_DEFAULT, "ukernel_wiphy_bss_get");

	/* ha a driver exportálja a saját debug-szintjét, magasra állítjuk a magas
	 * uServer-loglevelnél (hogy lássuk a hardver-init naplóját) */
	if (loglevel >= 6) {
		unsigned int *drv_log = dlsym(RTLD_DEFAULT, "rtw_drv_log_level");
		if (drv_log) { *drv_log = 5; printf("uServer: rtw_drv_log_level = 5\n"); }
	}

	/* Driver(s) given on argv (dev/one-shot): bring them up now. In chip-agnostic
	 * serve mode (no argv module) this is skipped — the guest's `modprobe` runs
	 * module_init + probe per driver it loads (UK_OP_MODPROBE -> modmgr). */
	if (nmodules > 0) {
		printf("uServer: module_init futtatása\n");
		if (A.run_inits()) { fprintf(stderr, "uServer: module_init hiba\n"); return 4; }

		printf("uServer: USB enumeráció + probe (%04x:%04x)\n", vid, pid);
		int bound = A.enumerate_and_probe(vid, pid);
		printf("uServer: bekötött eszközök: %d\n", bound);
		if (bound <= 0) { fprintf(stderr, "uServer: nincs bekötött eszköz\n"); A.run_exits(); return 5; }
	} else {
		printf("uServer: nincs argv-modul — serve mód (a guest modprobe-ja tölt drivert)\n");
	}

	if (A.wiphy_count && A.wiphy_count() > 0) {
		printf("uServer: cfg80211 wiphy-k:");
		for (size_t i = 0; i < A.wiphy_count(); i++) printf(" %s", A.wiphy_name(i));
		printf(" (scan a proxyn: UK_OP_SCAN)\n");
	}

	/* --up / --scan: a wlan interfész(ek) felhozása (ndo_open -> firmware + hw init) */
	if (do_up || do_scan) {
		int (*nd_count)(void) = dlsym(RTLD_DEFAULT, "ukernel_netdev_count");
		void *(*nd_get)(int) = dlsym(RTLD_DEFAULT, "ukernel_netdev_get");
		int (*nd_open)(void *) = dlsym(RTLD_DEFAULT, "ukernel_netdev_open");
		if (nd_count && nd_get && nd_open) {
			int nn = nd_count();
			printf("uServer: %d netdev felhozása (ndo_open)\n", nn);
			for (int i = 0; i < nn; i++) {
				void *d = nd_get(i);
				if (d) { int r = nd_open(d); printf("uServer:   netdev[%d] ndo_open -> %d\n", i, r); }
			}
		}
	}

	/* --scan: probe után közvetlen scan-próba (debug/bringup) */
	if (do_scan && A.wiphy_scan) {
		printf("uServer: --scan: ukernel_wiphy_scan(0)\n");
		int n = A.wiphy_scan(0);
		printf("uServer: scan -> %d BSS\n", n);
		for (int i = 0; i < n; i++) {
			struct uk_bss_info b;
			if (A.wiphy_bss_get(0, i, &b) == 0)
				printf("  [%d] SSID=\"%s\" %d dBm %d MHz\n", i, b.ssid, b.signal/100, b.freq);
		}
	}

	printf("uServer: KÉSZ — a driver fut, az eszköz be van kötve.\n");

	if (serve)
		proxy_serve(sockpath);

	A.run_exits();
	return 0;
}
