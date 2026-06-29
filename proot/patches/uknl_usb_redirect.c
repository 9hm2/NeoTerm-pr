/* NeoTerm USB (libusb) enablement — injected into proot's syscall/enter.c after
 * the block/fs/cam redirects. Gated by UK_USB.
 *
 * Goal: make UNMODIFIED distro libusb work under proot (no in-distro patched
 * libusb). On an Android device, stock libusb's init dies before it ever
 * enumerates:
 *
 *   [op_init] sysfs is available
 *   [linux_udev_start_event_monitor] could not initialize udev monitor
 *   [op_init] error starting hotplug event monitor
 *   unable to initialize libusb: -99
 *
 * libusb treats the hotplug (udev/netlink) monitor as fatal. The monitor is a
 * NETLINK_KOBJECT_UEVENT socket bound to a multicast group, which Android's
 * SELinux blocks for app-uid processes -> bind() fails -> libusb_init() == -99.
 *
 * Fix without touching libusb: fake success for exactly that bind() — an
 * AF_NETLINK socket binding to a non-zero multicast group. The monitor then
 * "starts" (events simply never arrive — fine, no kernel uevents under proot
 * anyway) and libusb_init() succeeds, after which enumeration proceeds via the
 * sysfs path that libusb already reports as available.
 *
 * Scope: only AF_NETLINK binds with nl_groups != 0 (i.e. monitors) are faked;
 * ordinary netlink request sockets (groups == 0) and all non-netlink binds pass
 * through untouched. This does mean a guest netlink *monitor* (e.g. `ip monitor`)
 * would also see a successful-but-silent bind; acceptable under the UK_USB gate.
 *
 * Not compiled standalone: uses enter.c's Tracee, peek_reg/poke_reg,
 * read_data, set_sysnum(PR_void), PR_bind, SYSARG_*, CURRENT.
 */
static int g_uk_usb = -1;
static int uk_usb_on(void)
{
	if (g_uk_usb < 0) { const char *e = getenv("UK_USB"); g_uk_usb = (e && *e && *e != '0') ? 1 : 0; }
	return g_uk_usb;
}

/* =====================================================================
 * Phase 2 — real USB device I/O (libusb usbfs path), no patched libusb.
 *
 * libusb enumerates over the faked /sys (Phase 1), then to actually talk to a
 * device it open()s /dev/bus/usb/BBB/DDD and drives it with USBDEVFS_* ioctls
 * (control/bulk/interrupt, claim, set-config, URBs, …). Under proot there is no
 * usbfs and no kernel access, so the guest opens an empty MARKER node bound at
 * that path; we recognise its fd and proxy every usbfs ioctl onto the REAL
 * usbfs fd that the Android app already holds (UsbDeviceConnection), handed to
 * us over the abstract socket "io.neoterm.usb" via SCM_RIGHTS. The Android fd is
 * a genuine kernel usbfs fd, so the proxied ioctls run natively — we only marshal
 * the argument structs and their data buffers between guest and tracer memory.
 *
 * This mirrors the camera shim (uknl_cam_redirect.c): per-(tgid,fd) state, detect
 * by readlink(/proc/pid/fd), trap ioctl/close/fstat at sysexit, set PR_void and
 * poke the result. Gated by UK_USB.
 * ===================================================================== */
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <linux/usbdevice_fs.h>
#include <stddef.h>
#include <poll.h>
#include <time.h>

/* Connect to the app's abstract usb fd-server. */
static int usb_conn(void)
{
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s < 0) return -1;
	struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX;
	const char *name = "io.neoterm.usb";
	a.sun_path[0] = '\0';
	size_t nl = strlen(name);
	memcpy(a.sun_path + 1, name, nl);
	socklen_t al = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + nl);
	if (connect(s, (struct sockaddr *)&a, al) < 0) { close(s); return -1; }
	return s;
}

/* Receive one fd (SCM_RIGHTS) plus the "OK …" header the server sends with it. */
static int usb_recv_fd(int s)
{
	struct msghdr msg; memset(&msg, 0, sizeof msg);
	char cbuf[CMSG_SPACE(sizeof(int))]; char dummy[256];
	struct iovec iov = { dummy, sizeof dummy };
	msg.msg_iov = &iov; msg.msg_iovlen = 1;
	msg.msg_control = cbuf; msg.msg_controllen = sizeof cbuf;
	ssize_t n = recvmsg(s, &msg, 0);
	if (n < 0) return -1;
	for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
		if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
			int fd = -1; memcpy(&fd, CMSG_DATA(c), sizeof fd); return fd;
		}
	return -1;
}

/* Acquire the real usbfs fd for /dev/bus/usb/BBB/DDD (deviceName == that path). */
static int usb_acquire(int bus, int dev)
{
	int s = usb_conn(); if (s < 0) return -1;
	char req[64]; int rn = snprintf(req, sizeof req, "/dev/bus/usb/%03d/%03d\n", bus, dev);
	if (write(s, req, (size_t)rn) != rn) { close(s); return -1; }
	int fd = usb_recv_fd(s);
	close(s);
	return fd;   /* -1 on no-permission/error */
}

/* Outstanding async URBs (USBDEVFS_SUBMITURB/REAPURB): the guest hands us a urb
 * whose ->buffer is a guest address and whose own address is what REAPURB must
 * return. We submit a tracer-local copy (local buffer) and remember the mapping. */
struct usb_urb {
	int used;
	int rfd;                 /* real fd it was submitted on (for matching) */
	word_t gurb;             /* guest address of the struct usbdevfs_urb */
	word_t gbuf;             /* guest address of the data buffer */
	int len;                 /* buffer_length */
	int is_in;               /* endpoint & 0x80 */
	struct usbdevfs_urb *lurb;   /* tracer-local urb (kernel holds this until reap) */
	void *lbuf;              /* tracer-local data buffer */
};
#define USB_MAXURB 256
static struct usb_urb g_usburb[USB_MAXURB];

/* Per-(tgid,fd) device state. */
static struct usb_fd {
	int used, pid, fd;
	int rfd;                 /* real usbfs fd from the app, or -1 if not yet/again */
	int tried;               /* acquire attempted (don't retry every ioctl) */
	int bus, dev;
} g_usbfd[64];
static int g_nusbfd = 0;

/* ukfs_tgid() is defined (static) by the FS redirect injected earlier in this
 * same translation unit, and reused here (as the camera shim does). */

static struct usb_fd *usbfd_get(int pid, int fd, int create)
{
	for (int i = 0; i < g_nusbfd; i++)
		if (g_usbfd[i].used && g_usbfd[i].pid == pid && g_usbfd[i].fd == fd) return &g_usbfd[i];
	if (!create) return NULL;
	int slot = -1;
	for (int i = 0; i < g_nusbfd; i++) if (!g_usbfd[i].used) { slot = i; break; }
	if (slot < 0) { if (g_nusbfd >= 64) return NULL; slot = g_nusbfd++; }
	struct usb_fd *U = &g_usbfd[slot];
	memset(U, 0, sizeof *U); U->used = 1; U->pid = pid; U->fd = fd; U->rfd = -1;
	return U;
}
static void usbfd_free(struct usb_fd *U)
{
	if (!U) return;
	/* drop any still-outstanding URBs on this real fd */
	for (int i = 0; i < USB_MAXURB; i++)
		if (g_usburb[i].used && g_usburb[i].rfd == U->rfd) {
			free(g_usburb[i].lbuf); free(g_usburb[i].lurb);
			memset(&g_usburb[i], 0, sizeof g_usburb[i]);
		}
	if (U->rfd >= 0) close(U->rfd);
	U->used = 0;
}

/* Path test: a usbfs device node — trailing "<bus>/<dev>" (numeric) under a
 * component that mentions "usb". Matches both the guest path /dev/bus/usb/BBB/DDD
 * (mid component "usb") and the bound host marker .../dev-bus-usb/BBB/DDD (mid
 * component "dev-bus-usb"), since readlink(/proc/pid/fd) gives the host path. */
static int usb_path_parse(const char *path, int *bus, int *dev)
{
	const char *e = path + strlen(path);
	if (e == path) return 0;
	const char *p2 = e;        while (p2 > path && p2[-1] != '/') p2--;   /* start of DDD */
	if (p2 == path) return 0;
	const char *s1 = p2 - 1;   const char *p1 = s1; while (p1 > path && p1[-1] != '/') p1--; /* start of BBB */
	if (p1 == path) return 0;
	const char *s0 = p1 - 1;   const char *p0 = s0; while (p0 > path && p0[-1] != '/') p0--; /* start of mid */
	int d = 0, nd = 0; for (const char *q = p2; q < e;  q++) { if (*q < '0' || *q > '9') return 0; d = d * 10 + (*q - '0'); nd++; }
	int b = 0, nb = 0; for (const char *q = p1; q < s1; q++) { if (*q < '0' || *q > '9') return 0; b = b * 10 + (*q - '0'); nb++; }
	if (nd == 0 || nb == 0) return 0;
	int usb = 0;
	for (const char *q = p0; q + 2 < s0; q++)
		if ((q[0]|32) == 'u' && (q[1]|32) == 's' && (q[2]|32) == 'b') { usb = 1; break; }
	if (!usb) return 0;
	*bus = b; *dev = d; return 1;
}
static int usb_fd_path(Tracee *tracee, int fd, char *out, size_t osz)
{
	char link[64]; snprintf(link, sizeof link, "/proc/%d/fd/%d", tracee->pid, fd);
	ssize_t n = readlink(link, out, osz - 1);
	if (n <= 0) return -1; out[n] = '\0'; return 0;
}
static int usb_is_fd(Tracee *tracee, int fd, int *bus, int *dev)
{
	char path[PATH_MAX];
	if (usb_fd_path(tracee, fd, path, sizeof path) < 0) return 0;
	return usb_path_parse(path, bus, dev);
}
static int usb_is_path(const char *p, int *bus, int *dev) { return usb_path_parse(p, bus, dev); }

/* Lazily get the real fd for a guest usb fd (acquire once). */
static int usb_real(Tracee *tracee, int fd, int bus, int dev)
{
	struct usb_fd *U = usbfd_get(ukfs_tgid(tracee->pid), fd, 1);
	if (!U) return -1;
	if (U->rfd >= 0) return U->rfd;
	if (U->tried) return -1;
	U->tried = 1; U->bus = bus; U->dev = dev;
	U->rfd = usb_acquire(bus, dev);
	return U->rfd;
}

static void usb_fill_stat(struct stat *st, int bus, int dev)
{
	memset(st, 0, sizeof *st);
	st->st_mode = S_IFCHR | 0660; st->st_nlink = 1;
	st->st_rdev = makedev(189, (unsigned)((bus - 1) * 128 + (dev - 1)));
	st->st_blksize = 4096; st->st_ino = 0xa5b00000ULL | (unsigned)((bus << 8) | dev);
}

#define USB_RET(v) do { poke_reg(tracee, SYSARG_RESULT, (word_t)(long)(v)); set_sysnum(tracee, PR_void); return true; } while (0)

/* Proxy a single usbfs ioctl. Returns the value to poke (>=0 ok, <0 -errno). */
static long usb_do_ioctl(Tracee *tracee, int rfd, unsigned long cmd, word_t arg)
{
	switch (cmd) {
	case USBDEVFS_GET_CAPABILITIES: {
		unsigned int caps = 0;
		if (ioctl(rfd, USBDEVFS_GET_CAPABILITIES, &caps) < 0) return -errno;
		caps &= ~(unsigned)USBDEVFS_CAP_MMAP;   /* force malloc'd buffers (no shared mmap) */
		write_data(tracee, arg, &caps, sizeof caps);
		return 0;
	}
	case USBDEVFS_GET_SPEED: {
		int sp = ioctl(rfd, USBDEVFS_GET_SPEED);
		return sp < 0 ? -errno : sp;
	}
	case USBDEVFS_CLAIMINTERFACE:
	case USBDEVFS_RELEASEINTERFACE: {
		unsigned int ifc = 0; read_data(tracee, &ifc, arg, sizeof ifc);
		return ioctl(rfd, cmd, &ifc) < 0 ? -errno : 0;
	}
	case USBDEVFS_SETCONFIGURATION: {
		unsigned int cfg = 0; read_data(tracee, &cfg, arg, sizeof cfg);
		return ioctl(rfd, USBDEVFS_SETCONFIGURATION, &cfg) < 0 ? -errno : 0;
	}
	case USBDEVFS_RESETEP:
	case USBDEVFS_CLEAR_HALT: {
		unsigned int ep = 0; read_data(tracee, &ep, arg, sizeof ep);
		return ioctl(rfd, cmd, &ep) < 0 ? -errno : 0;
	}
	case USBDEVFS_RESET:
		return ioctl(rfd, USBDEVFS_RESET) < 0 ? -errno : 0;
	case USBDEVFS_SETINTERFACE: {
		struct usbdevfs_setinterface si; memset(&si, 0, sizeof si);
		read_data(tracee, &si, arg, sizeof si);
		return ioctl(rfd, USBDEVFS_SETINTERFACE, &si) < 0 ? -errno : 0;
	}
	case USBDEVFS_GETDRIVER: {
		struct usbdevfs_getdriver gd; memset(&gd, 0, sizeof gd);
		read_data(tracee, &gd, arg, sizeof gd);
		if (ioctl(rfd, USBDEVFS_GETDRIVER, &gd) < 0) return -errno;
		write_data(tracee, arg, &gd, sizeof gd);
		return 0;
	}
	case USBDEVFS_CONNECTINFO: {
		struct usbdevfs_connectinfo ci; memset(&ci, 0, sizeof ci);
		if (ioctl(rfd, USBDEVFS_CONNECTINFO, &ci) < 0) return -errno;
		write_data(tracee, arg, &ci, sizeof ci);
		return 0;
	}
	case USBDEVFS_DISCONNECT_CLAIM: {
		struct usbdevfs_disconnect_claim dc; memset(&dc, 0, sizeof dc);
		read_data(tracee, &dc, arg, sizeof dc);
		if (ioctl(rfd, USBDEVFS_DISCONNECT_CLAIM, &dc) < 0) return -errno;
		write_data(tracee, arg, &dc, sizeof dc);
		return 0;
	}
	case USBDEVFS_IOCTL: {
		/* nested ioctl (driver disconnect/connect): the inner data, if any, is
		 * small/none for the usual USBDEVFS_DISCONNECT/CONNECT. Pass a NULL data. */
		struct usbdevfs_ioctl ui; memset(&ui, 0, sizeof ui);
		read_data(tracee, &ui, arg, sizeof ui);
		ui.data = NULL;
		return ioctl(rfd, USBDEVFS_IOCTL, &ui) < 0 ? -errno : 0;
	}
	case USBDEVFS_CONTROL: {
		struct usbdevfs_ctrltransfer ct; memset(&ct, 0, sizeof ct);
		read_data(tracee, &ct, arg, sizeof ct);
		int len = ct.wLength; void *buf = NULL;
		if (len > 0) {
			buf = malloc((size_t)len); if (!buf) return -ENOMEM;
			if (!(ct.bRequestType & 0x80))   /* host->device: copy out-data in */
				read_data(tracee, buf, (word_t)(uintptr_t)ct.data, (word_t)len);
		}
		void *gdata = ct.data; ct.data = buf;
		int rc = ioctl(rfd, USBDEVFS_CONTROL, &ct);
		int err = errno;
		if (rc > 0 && (ct.bRequestType & 0x80) && buf)   /* device->host: copy in-data back */
			write_data(tracee, (word_t)(uintptr_t)gdata, buf, (word_t)rc);
		free(buf);
		return rc < 0 ? -err : rc;
	}
	case USBDEVFS_BULK: {
		struct usbdevfs_bulktransfer bt; memset(&bt, 0, sizeof bt);
		read_data(tracee, &bt, arg, sizeof bt);
		int len = (int)bt.len; void *buf = NULL;
		int is_in = (bt.ep & 0x80) != 0;
		if (len > 0) {
			buf = malloc((size_t)len); if (!buf) return -ENOMEM;
			if (!is_in) read_data(tracee, buf, (word_t)(uintptr_t)bt.data, (word_t)len);
		}
		void *gdata = bt.data; bt.data = buf;
		int rc = ioctl(rfd, USBDEVFS_BULK, &bt);
		int err = errno;
		if (rc > 0 && is_in && buf) write_data(tracee, (word_t)(uintptr_t)gdata, buf, (word_t)rc);
		free(buf);
		return rc < 0 ? -err : rc;
	}
	case USBDEVFS_SUBMITURB: {
		struct usbdevfs_urb u; memset(&u, 0, sizeof u);
		read_data(tracee, &u, arg, sizeof u);
		int slot = -1;
		for (int i = 0; i < USB_MAXURB; i++) if (!g_usburb[i].used) { slot = i; break; }
		if (slot < 0) return -ENOMEM;
		int is_in = (u.endpoint & 0x80) != 0;
		int blen = u.buffer_length;
		void *lbuf = NULL;
		if (blen > 0) {
			lbuf = malloc((size_t)blen); if (!lbuf) return -ENOMEM;
			if (!is_in) read_data(tracee, lbuf, (word_t)(uintptr_t)u.buffer, (word_t)blen);
		}
		/* a private copy the kernel keeps referencing until we reap it */
		struct usbdevfs_urb *lurb = malloc(sizeof *lurb);
		if (!lurb) { free(lbuf); return -ENOMEM; }
		*lurb = u; lurb->buffer = lbuf;
		if (ioctl(rfd, USBDEVFS_SUBMITURB, lurb) < 0) { int e = errno; free(lbuf); free(lurb); return -e; }
		struct usb_urb *T = &g_usburb[slot];
		T->used = 1; T->rfd = rfd; T->gurb = arg; T->gbuf = (word_t)(uintptr_t)u.buffer;
		T->len = blen; T->is_in = is_in; T->lurb = lurb; T->lbuf = lbuf;
		return 0;
	}
	case USBDEVFS_REAPURB:
	case USBDEVFS_REAPURBNDELAY: {
		void *reaped = NULL;
		if (ioctl(rfd, cmd, &reaped) < 0) return -errno;
		struct usb_urb *T = NULL;
		for (int i = 0; i < USB_MAXURB; i++)
			if (g_usburb[i].used && g_usburb[i].lurb == reaped) { T = &g_usburb[i]; break; }
		if (!T) return -EINVAL;
		/* copy completed fields + IN data back into the guest's urb + buffer */
		struct usbdevfs_urb gu; read_data(tracee, &gu, T->gurb, sizeof gu);
		gu.status = T->lurb->status;
		gu.actual_length = T->lurb->actual_length;
		gu.error_count = T->lurb->error_count;
		gu.start_frame = T->lurb->start_frame;
		write_data(tracee, T->gurb, &gu, sizeof gu);
		if (T->is_in && T->lbuf && T->lurb->actual_length > 0) {
			int al = T->lurb->actual_length; if (al > T->len) al = T->len;
			write_data(tracee, T->gbuf, T->lbuf, (word_t)al);
		}
		/* REAPURB returns the urb pointer via the void** arg — give the guest back
		 * its OWN urb address (what it passed to SUBMITURB). */
		word_t gurb = T->gurb;
		write_data(tracee, arg, &gurb, sizeof gurb);
		free(T->lbuf); free(T->lurb); memset(T, 0, sizeof *T);
		return 0;
	}
	case USBDEVFS_DISCARDURB: {
		/* arg is the guest urb address */
		struct usb_urb *T = NULL;
		for (int i = 0; i < USB_MAXURB; i++)
			if (g_usburb[i].used && g_usburb[i].rfd == rfd && g_usburb[i].gurb == arg) { T = &g_usburb[i]; break; }
		if (!T) return -EINVAL;
		int rc = ioctl(rfd, USBDEVFS_DISCARDURB, T->lurb);
		return rc < 0 ? -errno : 0;   /* the urb is reaped (with -ECONNRESET) afterwards */
	}
	default:
		/* Unknown/unsupported usbfs ioctl: report ENOTTY like the kernel would for
		 * a non-usbfs file, so callers degrade instead of hanging. */
		return -ENOTTY;
	}
}

/* AF_NETLINK=16, NETLINK_KOBJECT_UEVENT=15, NETLINK_ROUTE=0,
 * SOL_NETLINK=270, NETLINK_ADD_MEMBERSHIP=1. */
static bool uknl_usb_dispatch(Tracee *tracee, word_t nr)
{
	if (!uk_usb_on()) return false;

	/* ---- device-node I/O: stat / ioctl / close on /dev/bus/usb/BBB/DDD ---- */
	int bus, dev;
	if (nr == PR_newfstatat || nr == PR_fstatat64) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_2);
		char gp[PATH_MAX];
		if (pa && read_string(tracee, gp, pa, sizeof gp) > 0 && usb_is_path(gp, &bus, &dev)) {
			struct stat st; usb_fill_stat(&st, bus, dev);
			write_data(tracee, peek_reg(tracee, CURRENT, SYSARG_3), &st, sizeof st);
			USB_RET(0);
		}
		return false;
	}
	if (nr == PR_fstat || nr == PR_fstat64) {
		int sfd = (int) peek_reg(tracee, CURRENT, SYSARG_1);
		if (usb_is_fd(tracee, sfd, &bus, &dev)) {
			struct stat st; usb_fill_stat(&st, bus, dev);
			write_data(tracee, peek_reg(tracee, CURRENT, SYSARG_2), &st, sizeof st);
			USB_RET(0);
		}
		return false;
	}
	if (nr == PR_close) {
		int fd = (int) peek_reg(tracee, CURRENT, SYSARG_1);
		struct usb_fd *U = usbfd_get(ukfs_tgid(tracee->pid), fd, 0);
		if (U) usbfd_free(U);
		return false;   /* let the real close run on the marker fd */
	}
	if (nr == PR_ioctl) {
		int fd = (int) peek_reg(tracee, CURRENT, SYSARG_1);
		if (!usb_is_fd(tracee, fd, &bus, &dev)) return false;
		unsigned long cmd = (unsigned long) peek_reg(tracee, CURRENT, SYSARG_2);
		word_t arg = peek_reg(tracee, CURRENT, SYSARG_3);
		int rfd = usb_real(tracee, fd, bus, dev);
		if (rfd < 0) USB_RET(-EACCES);   /* no permission / app not serving it */
		long rc = usb_do_ioctl(tracee, rfd, cmd, arg);
		USB_RET(rc);
	}

	/* poll()/ppoll() over usbfs fds: the guest is polling EMPTY marker files (a
	 * regular file is always "ready"), so libusb's event loop would busy-spin
	 * REAPURBNDELAY. usbfs gates POLLOUT on a URB actually being reapable, so poll
	 * the REAL fds instead — byte-for-byte what libusb expects on Linux. Any
	 * non-usb fd in the same call (libusb's internal event pipe) is reported
	 * not-ready and the timeout is capped at 100ms so it is rechecked promptly. */
	if (nr == PR_poll || nr == PR_ppoll) {
		unsigned long nfds = (unsigned long) peek_reg(tracee, CURRENT, SYSARG_2);
		if (nfds == 0 || nfds > 16) return false;
		word_t fds_addr = peek_reg(tracee, CURRENT, SYSARG_1);
		struct pollfd pfds[16];
		if (read_data(tracee, pfds, fds_addr, nfds * sizeof(struct pollfd)) < 0) return false;
		struct pollfd local[16]; int map[16]; int nl = 0, nusb = 0;
		for (unsigned i = 0; i < nfds; i++) {
			pfds[i].revents = 0;
			int b, d;
			if (usb_is_fd(tracee, pfds[i].fd, &b, &d)) {
				nusb++;
				int rfd = usb_real(tracee, pfds[i].fd, b, d);
				if (rfd >= 0) { local[nl].fd = rfd; local[nl].events = pfds[i].events; local[nl].revents = 0; map[nl] = i; nl++; }
			}
		}
		if (nusb == 0) return false;   /* nothing of ours -> let proot handle it */
		int req = (nr == PR_poll) ? (int) peek_reg(tracee, CURRENT, SYSARG_3) : 100;
		if (nr == PR_ppoll) {
			word_t ts = peek_reg(tracee, CURRENT, SYSARG_3);
			if (!ts) req = -1; else { struct timespec t; if (read_data(tracee, &t, ts, sizeof t) == 0) req = (int)(t.tv_sec * 1000 + t.tv_nsec / 1000000); }
		}
		if (req < 0 || req > 100) req = 100;   /* cap so the event pipe is rechecked */
		int ready = nl ? poll(local, (nfds_t) nl, req) : (nanosleep(&(struct timespec){0, (long)req * 1000000L}, NULL), 0);
		int n = 0;
		for (int i = 0; i < nl; i++) if (local[i].revents) { pfds[map[i]].revents = local[i].revents; n++; }
		(void) ready;
		write_data(tracee, fds_addr, pfds, nfds * sizeof(struct pollfd));
		USB_RET(n);
	}

	/* 1) socket(AF_NETLINK, *, NETLINK_KOBJECT_UEVENT): Android/SELinux blocks
	 * creating a uevent netlink socket for app uids, so udev_monitor_new_from_netlink
	 * returns NULL and libusb_init() fails. Rewrite the protocol to NETLINK_ROUTE
	 * (which app uids may create) so the socket() succeeds and libusb gets a real,
	 * pollable fd. We then neutralise its membership below so no events ever flow. */
	if (nr == PR_socket) {
		int domain   = (int) peek_reg(tracee, CURRENT, SYSARG_1);
		int protocol = (int) peek_reg(tracee, CURRENT, SYSARG_3);
		if (domain == 16 && protocol == 15) {
			poke_reg(tracee, SYSARG_3, 0 /* NETLINK_ROUTE */);
			/* let it run with the rewritten protocol */
		}
		return false;
	}

	/* 2) bind(AF_NETLINK, nl_groups != 0): the hotplug monitor's group bind. Fake
	 * success so the (rewritten) socket isn't actually subscribed to anything. */
	if (nr == PR_bind) {
		word_t addr = peek_reg(tracee, CURRENT, SYSARG_2);
		word_t len  = peek_reg(tracee, CURRENT, SYSARG_3);
		if (!addr || len < 12) return false;            /* sockaddr_nl is 12 bytes */
		unsigned char sa[12];
		if (read_data(tracee, sa, addr, sizeof sa) < 0) return false;
		unsigned short fam = (unsigned short)(sa[0] | (sa[1] << 8));
		unsigned int groups = (unsigned)(sa[8] | (sa[9] << 8) | (sa[10] << 16) | (sa[11] << 24));
		if (fam != 16 || groups == 0) return false;
		poke_reg(tracee, SYSARG_RESULT, 0);
		set_sysnum(tracee, PR_void);
		return true;
	}

	/* 3) setsockopt(SOL_NETLINK, NETLINK_ADD_MEMBERSHIP): subscribing to the uevent
	 * multicast group. Fake success so enable_receiving() doesn't fail; with no real
	 * membership the monitor fd just never becomes readable (no hotplug — fine). */
	if (nr == PR_setsockopt) {
		int level   = (int) peek_reg(tracee, CURRENT, SYSARG_2);
		int optname = (int) peek_reg(tracee, CURRENT, SYSARG_3);
		if (level != 270 || optname != 1) return false;
		poke_reg(tracee, SYSARG_RESULT, 0);
		set_sysnum(tracee, PR_void);
		return true;
	}

	return false;
}
