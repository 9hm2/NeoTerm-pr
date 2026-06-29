/* uKernel — USBFS HCD backend (libusb NÉLKÜL).
 *
 * A libusb (ezen az aarch64 1.0.x buildjén) az eseményhurkában nemdeterminisztikusan elszáll.
 * Ehelyett KÖZVETLENÜL a kernel usbfs-ét használjuk: az eszköz fd-jét a NeoTerm abstract socketről
 * (io.neoterm.usb) kérjük (SCM_RIGHTS), majd USBDEVFS_* ioctl-okkal beszélünk vele:
 *   - control transfer: USBDEVFS_CONTROL (SZINKRON ioctl -> nincs event-loop, nincs crash)
 *   - bulk/int async:   USBDEVFS_SUBMITURB + USBDEVFS_REAPURBNDELAY (a kernel natív async-ja)
 * Belépő: ukernel_hcd_usbfs().  (Ha a /dev/bus/usb közvetlenül elérhető, az is megnyitható: UK_USBFS_DEV.)
 */
#include "ukernel/hcd.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/usbdevice_fs.h>

struct ukernel_hcd {
	int fd;                  /* az usbfs eszköz-fd (a NeoTerm-socketről vagy közvetlen) */
	uint8_t dev_desc[18];  int dev_desc_len;
	uint8_t cfg_desc[1024]; int cfg_desc_len;
	int claimed;
};
static struct ukernel_hcd inst;

static void lf(const char *f, ...) { va_list a; va_start(a, f); fprintf(stderr, "uKernel/usbfs: "); vfprintf(stderr, f, a); va_end(a); }

/* ---- NeoTerm abstract socket: egy parancs elküldése, opcionálisan fd fogadása (SCM_RIGHTS) ---- */
static int neoterm_connect(void)
{
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s < 0) return -1;
	struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX;
	static const char nm[] = "io.neoterm.usb";
	a.sun_path[0] = '\0'; memcpy(a.sun_path + 1, nm, sizeof(nm) - 1);
	socklen_t L = offsetof(struct sockaddr_un, sun_path) + 1 + (sizeof(nm) - 1);
	if (connect(s, (struct sockaddr *)&a, L) < 0) { close(s); return -1; }
	return s;
}

/* LIST: a [granted] eszközök; megkeressük a vid:pid-hez tartozó /dev/bus/usb/B/D útvonalat */
static int neoterm_find_path(uint16_t vid, uint16_t pid, char *path, size_t cap)
{
	int s = neoterm_connect(); if (s < 0) return -1;
	if (write(s, "LIST\n", 5) != 5) { close(s); return -1; }
	char buf[8192]; size_t off = 0; ssize_t n;
	struct timeval tv = { 2, 0 }; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	while (off < sizeof(buf) - 1 && (n = read(s, buf + off, sizeof(buf) - 1 - off)) > 0) off += (size_t)n;
	close(s); buf[off] = 0;
	char want[16]; snprintf(want, sizeof want, "%04x:%04x", vid, pid);
	char *save = NULL;
	for (char *ln = strtok_r(buf, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
		if (strncmp(ln, "/dev/bus/usb/", 13)) continue;
		char p[128], idv[32]; if (sscanf(ln, "%127s %31s", p, idv) < 2) continue;
		if (strcasecmp(idv, want) == 0) { snprintf(path, cap, "%s", p); return 0; }
	}
	return -1;
}

/* path -> usbfs fd (SCM_RIGHTS-szal) a NeoTerm-socketről */
static int neoterm_get_fd(const char *path)
{
	int s = neoterm_connect(); if (s < 0) return -1;
	char line[80]; int ln = snprintf(line, sizeof line, "%s\n", path);
	if (write(s, line, ln) != ln) { close(s); return -1; }
	struct msghdr m; memset(&m, 0, sizeof m);
	char data[256]; struct iovec io = { data, sizeof data - 1 };
	union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } cm; memset(&cm, 0, sizeof cm);
	m.msg_iov = &io; m.msg_iovlen = 1; m.msg_control = cm.buf; m.msg_controllen = sizeof cm.buf;
	ssize_t n = recvmsg(s, &m, 0); int fd = -1;
	if (n > 0) for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
		if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) { memcpy(&fd, CMSG_DATA(c), sizeof fd); break; }
	close(s);
	if (fd >= 0) lseek(fd, 0, SEEK_SET);   /* a fd-offset megosztott — a descriptorokhoz visszatekerünk */
	return fd;
}

/* ---- HCD ops ---- */
static struct ukernel_hcd *m_open(uint16_t vid, uint16_t pid)
{
	struct ukernel_hcd *d = &inst; memset(d, 0, sizeof *d); d->fd = -1;
	const char *direct = getenv("UK_USBFS_DEV");
	if (direct) {
		d->fd = open(direct, O_RDWR);
		if (d->fd < 0) { lf("közvetlen open(%s) hiba: %s\n", direct, strerror(errno)); return NULL; }
	} else {
		char path[128];
		if (neoterm_find_path(vid, pid, path, sizeof path) < 0) { lf("nincs %04x:%04x a NeoTerm LIST-ben (granted?)\n", vid, pid); return NULL; }
		d->fd = neoterm_get_fd(path);
		if (d->fd < 0) { lf("fd-szerzés hiba (%s)\n", path); return NULL; }
		lf("megnyitva %04x:%04x a NeoTerm-socketről (%s, fd=%d)\n", vid, pid, path, d->fd);
	}
	/* Opcionális port-reset (UK_USBFS_RESET=1): a flaky adapter bulk-RX-e néha
	 * bewedge-elődik sok claim/release-ciklus után — a USBDEVFS_RESET tiszta
	 * chip-állapotot ad fizikai újradugás nélkül. (A NeoTerm-fd túléli, mert a
	 * busz-cím nem feltétlen változik; ha mégis, a hívó újra-enumerál.) */
	if (getenv("UK_USBFS_RESET")) {
		int rr = ioctl(d->fd, USBDEVFS_RESET, NULL);
		lf("USBDEVFS_RESET -> %d%s\n", rr, rr < 0 ? strerror(errno) : "");
	}
	/* descriptorok beolvasása a fd-ről: device(18) + a teljes config-blob(ok) egymás után */
	uint8_t blob[1100]; lseek(d->fd, 0, SEEK_SET);
	int got = (int)read(d->fd, blob, sizeof blob);
	if (got >= 18) {
		memcpy(d->dev_desc, blob, 18); d->dev_desc_len = 18;
		if (got >= 18 + 4 && blob[18 + 1] == 2) {   /* config descriptor követi */
			int tot = blob[18 + 2] | (blob[18 + 3] << 8);
			if (tot > 0 && 18 + tot <= got && tot <= (int)sizeof d->cfg_desc) { memcpy(d->cfg_desc, blob + 18, tot); d->cfg_desc_len = tot; }
		}
	}
	lf("descriptorok: dev=%d cfg=%d bájt\n", d->dev_desc_len, d->cfg_desc_len);
	return (d->dev_desc_len >= 18) ? d : NULL;
}
static int m_dev(struct ukernel_hcd *d, const uint8_t **b, int *l) { *b = d->dev_desc; *l = d->dev_desc_len; return d->dev_desc_len >= 18 ? 0 : -EIO; }
static int m_cfg(struct ukernel_hcd *d, int i, const uint8_t **b, int *l) { (void)i; *b = d->cfg_desc; *l = d->cfg_desc_len; return d->cfg_desc_len >= 9 ? 0 : -EIO; }
static int m_setcfg(struct ukernel_hcd *d, int c) { unsigned cfg = c; ioctl(d->fd, USBDEVFS_SETCONFIGURATION, &cfg); return 0; }   /* best-effort */
static int m_claim(struct ukernel_hcd *d, int iface)
{
	unsigned i = iface;
	if (ioctl(d->fd, USBDEVFS_CLAIMINTERFACE, &i) == 0) { d->claimed = 1; return 0; }
	/* foglalt -> a kernel-driver leválasztása, majd újra */
	struct usbdevfs_ioctl c = { .ifno = iface, .ioctl_code = USBDEVFS_DISCONNECT, .data = NULL };
	ioctl(d->fd, USBDEVFS_IOCTL, &c);
	if (ioctl(d->fd, USBDEVFS_CLAIMINTERFACE, &i) == 0) { d->claimed = 1; return 0; }
	return 0;   /* best-effort: a NeoTerm-fd gyakran már szabad interfésszel jön */
}
static int m_setif(struct ukernel_hcd *d, int iface, int alt) { struct usbdevfs_setinterface s = { (unsigned)iface, (unsigned)alt }; ioctl(d->fd, USBDEVFS_SETINTERFACE, &s); return 0; }

static int g_trace = -1;
static int usbfs_trace(void) { if (g_trace < 0) g_trace = getenv("UK_USBFS_TRACE") ? 1 : 0; return g_trace; }

static int m_xfer(struct ukernel_hcd *d, struct ukernel_xfer *x)   /* SZINKRON control/bulk ioctl */
{
	if (x->type == UK_XFER_CONTROL) {
		struct usbdevfs_ctrltransfer c = {
			.bRequestType = x->bmRequestType, .bRequest = x->bRequest,
			.wValue = x->wValue, .wIndex = x->wIndex, .wLength = (uint16_t)(x->wLength ? x->wLength : x->length),
			.timeout = x->timeout_ms ? x->timeout_ms : 1000, .data = x->data,
		};
		if (usbfs_trace()) lf("CTRL bmReq=0x%02x bReq=0x%02x wVal=0x%04x wIdx=0x%04x wLen=%u\n", c.bRequestType, c.bRequest, c.wValue, c.wIndex, c.wLength);
		int r = ioctl(d->fd, USBDEVFS_CONTROL, &c);
		if (usbfs_trace()) lf("  CTRL -> r=%d errno=%d\n", r, r < 0 ? errno : 0);
		if (r < 0) return -errno;
		x->actual_length = r; return 0;
	}
	struct usbdevfs_bulktransfer b = { .ep = x->ep, .len = (unsigned)x->length, .timeout = x->timeout_ms ? x->timeout_ms : 1000, .data = x->data };
	if (usbfs_trace()) lf("BULK ep=0x%02x len=%u\n", b.ep, b.len);
	int r = ioctl(d->fd, USBDEVFS_BULK, &b);
	if (usbfs_trace()) lf("  BULK -> r=%d errno=%d\n", r, r < 0 ? errno : 0);
	if (r < 0) return -errno;
	x->actual_length = r; return 0;
}

/* ---- async: USBDEVFS_SUBMITURB + REAPURBNDELAY ----
 * Az urb-OBJEKTUMOT ÚJRAHASZNÁLJUK (egy a->backend per ukernel_async): nem calloc+free minden
 * cikluson, így nincs free-verseny/double-free a pump-szálon, és a memória korlátos. */
static int m_submit(struct ukernel_hcd *d, struct ukernel_async *a)
{
	struct usbdevfs_urb *u = a->backend;
	if (!u) { u = calloc(1, sizeof *u + 64); if (!u) return -ENOMEM; a->backend = u; }   /* + padding */
	memset(u, 0, sizeof *u);
	u->type = (a->xfer.type == UK_XFER_INT) ? USBDEVFS_URB_TYPE_INTERRUPT : USBDEVFS_URB_TYPE_BULK;
	u->endpoint = a->xfer.ep;
	u->buffer = a->xfer.data; u->buffer_length = a->xfer.length;
	u->usercontext = a;
	if (usbfs_trace()) lf("SUBMITURB type=%d ep=0x%02x len=%d\n", u->type, u->endpoint, u->buffer_length);
	if (ioctl(d->fd, USBDEVFS_SUBMITURB, u) < 0) { if (usbfs_trace()) lf("  SUBMITURB errno=%d\n", errno); return -errno; }   /* a->backend megmarad */
	return 0;
}
static int m_cancel(struct ukernel_hcd *d, struct ukernel_async *a)
{ if (a->backend) ioctl(d->fd, USBDEVFS_DISCARDURB, a->backend); return 0; }

static int m_pump(struct ukernel_hcd *d, int timeout_ms)
{
	int budget = timeout_ms > 0 ? timeout_ms : 1, did = 0;
	for (;;) {
		struct usbdevfs_urb *u = NULL;
		if (ioctl(d->fd, USBDEVFS_REAPURBNDELAY, &u) == 0 && u) {
			if (usbfs_trace()) lf("REAP ep=0x%02x actual=%d status=%d\n", u->endpoint, u->actual_length, u->status);
			struct ukernel_async *a = u->usercontext;
			if (a) {
				a->xfer.actual_length = u->actual_length;
				a->status = (u->status == 0) ? 0 : -EIO;
				if (a->complete) a->complete(a, a->user);   /* újra-submittálhat -> az a->backend urb-ot újrahasználja */
			}
			did = 1; continue;   /* NINCS free — az urb-objektum újrahasználódik */
		}
		if (did || budget <= 0) break;
		struct timespec ts = { 0, 1000 * 1000 }; nanosleep(&ts, NULL); budget--;   /* 1 ms */
	}
	return 0;
}
static void m_close(struct ukernel_hcd *d)
{
	if (!d || d->fd < 0) return;
	if (d->claimed) { unsigned i = 0; ioctl(d->fd, USBDEVFS_RELEASEINTERFACE, &i); d->claimed = 0; }
	close(d->fd); d->fd = -1;   /* teljes elengedés -> a class-shim (FS/net-bridge) megnyithatja */
}

static const struct ukernel_hcd_ops usbfs_ops = {
	.name = "usbfs", .open = m_open,
	.get_device_descriptor = m_dev, .get_config_descriptor = m_cfg,
	.set_configuration = m_setcfg, .claim_interface = m_claim, .set_interface = m_setif,
	.xfer = m_xfer, .submit = m_submit, .cancel = m_cancel, .pump_events = m_pump, .close = m_close,
};
const struct ukernel_hcd_ops *ukernel_hcd_usbfs(void) { return &usbfs_ops; }
