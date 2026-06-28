/* NeoTerm V4L2 camera redirect — injected verbatim into proot's
 * syscall/enter.c (just before translate_syscall_enter) by fakeid0-xattr.py,
 * after the block proxy + FUSE/fs redirect so it can reuse their socket
 * helpers (uksd_wn/uksd_rn/uksd_rl) and enter.c scope.
 *
 * The Android CameraBridge can't expose a real /dev/video0 (no root, no
 * v4l2loopback), only an MJPEG stream. This shim makes /dev/video0 a *real*
 * V4L2 capture device to the guest: it is a bound empty marker file, and every
 * V4L2 ioctl / read() on its fd is emulated here, pulling frames from the app
 * over the abstract unix socket "io.neoterm.camera". So OpenCV's
 * VideoCapture(0), ffmpeg -f v4l2, GStreamer v4l2src, cheese, guvcview, … all
 * open the camera natively — not just URL-aware apps.
 *
 * I/O methods, all no-root: MMAP (the guest mmaps the marker file directly; on
 * DQBUF we pwrite() the frame into the file at the buffer offset, so the guest's
 * MAP_SHARED mapping sees it — no mmap interception needed), USERPTR (frame
 * copied into the guest buffer via write_data) and READ. Controls (brightness,
 * autofocus, …) are proxied to Camera2 via CTRL_* and so are exposed too.
 *
 * Gated by UK_CAM=1 (set by ProotManager when the camera is enabled). Not
 * compiled standalone: relies on enter.c's Tracee, peek_reg/poke_reg,
 * set_sysnum(PR_void), read_data/write_data/read_string, the PR_ and SYSARG_
 * enums, and the block proxy's uksd_wn/uksd_rn/uksd_rl. Uses kernel UAPI videodev2.h
 * (present in both the host build and the NDK sysroot, so struct layouts are the
 * real ones — no hand-coded offsets).
 */
#include <linux/videodev2.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <poll.h>
/* ---- gating: UK_CAM=1 ---- */
static int g_uk_cam = -1;
static int uk_cam_on(void)
{
	if (g_uk_cam < 0) { const char *e = getenv("UK_CAM"); g_uk_cam = (e && *e && *e != '0') ? 1 : 0; }
	return g_uk_cam;
}

/* ---- io.neoterm.camera connection (line cmds + binary frame payloads) ---- */
static int g_cam_sock = -1;
static int cam_conn(void)
{
	if (g_cam_sock >= 0) return g_cam_sock;
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s < 0) return -1;
	struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX;
	const char *name = "io.neoterm.camera";
	a.sun_path[0] = '\0';
	size_t nl = strlen(name);
	if (nl > sizeof(a.sun_path) - 1) nl = sizeof(a.sun_path) - 1;
	memcpy(a.sun_path + 1, name, nl);
	socklen_t al = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + nl);
	if (connect(s, (struct sockaddr *)&a, al) < 0) { close(s); return -1; }
	g_cam_sock = s; return s;
}
static void cam_drop(void) { if (g_cam_sock >= 0) { close(g_cam_sock); g_cam_sock = -1; } }

/* ---- capability cache (filled once from the CAPS reply) ---- */
struct cam_size { int w, h, fn, fd; };
struct cam_fmt  { unsigned fourcc; char tag[8]; int nsz; struct cam_size sz[16]; };
static struct cam_fmt g_camfmt[8];
static int g_ncamfmt = -1;

static unsigned cam_fourcc(const char *t)
{
	unsigned f = 0; for (int i = 0; i < 4 && t[i]; i++) f |= ((unsigned)(unsigned char)t[i]) << (8 * i);
	return f;
}
static int cam_caps(void)
{
	if (g_ncamfmt >= 0) return g_ncamfmt > 0 ? 0 : -1;
	g_ncamfmt = 0;
	int s = cam_conn(); if (s < 0) return -1;
	if (uksd_wn(s, "CAPS\n", 5) < 0) { cam_drop(); return -1; }
	char line[128]; if (uksd_rl(s, line, sizeof line) < 0) { cam_drop(); return -1; }
	int nf; if (sscanf(line, "OK %d", &nf) != 1) return -1;
	if (nf > 8) nf = 8;
	for (int i = 0; i < nf; i++) {
		if (uksd_rl(s, line, sizeof line) < 0) { cam_drop(); return -1; }
		char tag[16]; int ns = 0;
		if (sscanf(line, "%15s %d", tag, &ns) < 1) return -1;
		struct cam_fmt *F = &g_camfmt[g_ncamfmt];
		memset(F, 0, sizeof *F);
		snprintf(F->tag, sizeof F->tag, "%s", tag);
		F->fourcc = cam_fourcc(tag);
		if (ns > 16) ns = 16;
		for (int j = 0; j < ns; j++) {
			if (uksd_rl(s, line, sizeof line) < 0) { cam_drop(); return -1; }
			struct cam_size *Z = &F->sz[F->nsz];
			Z->fn = 30; Z->fd = 1;
			if (sscanf(line, "%d %d %d %d", &Z->w, &Z->h, &Z->fn, &Z->fd) >= 2) F->nsz++;
		}
		g_ncamfmt++;
	}
	return g_ncamfmt > 0 ? 0 : -1;
}
static struct cam_fmt *cam_find_fmt(unsigned fourcc)
{
	if (cam_caps() < 0) return NULL;
	for (int i = 0; i < g_ncamfmt; i++) if (g_camfmt[i].fourcc == fourcc) return &g_camfmt[i];
	return NULL;
}
/* Snap (w,h) to the nearest supported size of fmt F (by pixel-count distance). */
static void cam_snap_size(struct cam_fmt *F, int *w, int *h)
{
	if (!F || F->nsz == 0) return;
	long want = (long)*w * *h, best = -1; int bi = 0;
	for (int i = 0; i < F->nsz; i++) {
		long d = (long)F->sz[i].w * F->sz[i].h - want; if (d < 0) d = -d;
		if (best < 0 || d < best) { best = d; bi = i; }
	}
	*w = F->sz[bi].w; *h = F->sz[bi].h;
}

static int cam_start(const char *tag, int w, int h)
{
	int s = cam_conn(); if (s < 0) return -1;
	char req[64]; int n = snprintf(req, sizeof req, "START %s %d %d\n", tag, w, h);
	if (uksd_wn(s, req, n) < 0) { cam_drop(); return -1; }
	char line[32]; if (uksd_rl(s, line, sizeof line) < 0) { cam_drop(); return -1; }
	return (line[0] == 'O' && line[1] == 'K') ? 0 : -1;
}
static void cam_stop(void)
{
	int s = cam_conn(); if (s < 0) return;
	if (uksd_wn(s, "STOP\n", 5) < 0) { cam_drop(); return; }
	char line[16]; if (uksd_rl(s, line, sizeof line) < 0) cam_drop();
}
/* Fetch one frame into the static grow-buffer; *out/*len point into it. */
static unsigned char *g_camframe = NULL; static size_t g_camframe_cap = 0;
static int cam_frame(int tmo, unsigned char **out, int *len)
{
	int s = cam_conn(); if (s < 0) return -1;
	char req[32]; int n = snprintf(req, sizeof req, "FRAME %d\n", tmo);
	if (uksd_wn(s, req, n) < 0) { cam_drop(); return -1; }
	char line[32]; if (uksd_rl(s, line, sizeof line) < 0) { cam_drop(); return -1; }
	int fl; if (sscanf(line, "OK %d", &fl) != 1 || fl < 0) return -1;
	if ((size_t)fl > g_camframe_cap) {
		unsigned char *nb = realloc(g_camframe, (size_t)fl); if (!nb) return -1;
		g_camframe = nb; g_camframe_cap = (size_t)fl;
	}
	if (fl > 0 && uksd_rn(s, g_camframe, (size_t)fl) < 0) { cam_drop(); return -1; }
	*out = g_camframe; *len = fl; return 0;
}

/* ---- controls (proxied to Camera2 via CTRL_*) ---- */
struct cam_ctrl { unsigned id; int type, min, max, step, def, flags; char name[32]; };
static struct cam_ctrl g_camctrl[32]; static int g_ncamctrl = -1;
static int cam_ctrl_list(void)
{
	if (g_ncamctrl >= 0) return g_ncamctrl > 0 ? 0 : -1;
	g_ncamctrl = 0;
	int s = cam_conn(); if (s < 0) return -1;
	if (uksd_wn(s, "CTRL_LIST\n", 10) < 0) { cam_drop(); return -1; }
	char line[160]; if (uksd_rl(s, line, sizeof line) < 0) { cam_drop(); return -1; }
	int n; if (sscanf(line, "OK %d", &n) != 1) return -1;
	if (n > 32) n = 32;
	for (int i = 0; i < n; i++) {
		if (uksd_rl(s, line, sizeof line) < 0) { cam_drop(); return -1; }
		struct cam_ctrl *C = &g_camctrl[g_ncamctrl];
		char nm[64] = {0}; int consumed = 0;
		if (sscanf(line, "%u %d %d %d %d %d %d %n", &C->id, &C->type, &C->min, &C->max,
		           &C->step, &C->def, &C->flags, &consumed) >= 7) {
			snprintf(nm, sizeof nm, "%s", consumed > 0 ? line + consumed : "");
			snprintf(C->name, sizeof C->name, "%s", nm);
			g_ncamctrl++;
		}
	}
	return g_ncamctrl > 0 ? 0 : -1;
}
static struct cam_ctrl *cam_ctrl_find(unsigned id)
{
	if (cam_ctrl_list() < 0) return NULL;
	for (int i = 0; i < g_ncamctrl; i++) if (g_camctrl[i].id == id) return &g_camctrl[i];
	return NULL;
}
/* Smallest control id strictly greater than `id` (for V4L2_CTRL_FLAG_NEXT_CTRL). */
static struct cam_ctrl *cam_ctrl_next(unsigned id)
{
	if (cam_ctrl_list() < 0) return NULL;
	struct cam_ctrl *best = NULL;
	for (int i = 0; i < g_ncamctrl; i++)
		if (g_camctrl[i].id > id && (!best || g_camctrl[i].id < best->id)) best = &g_camctrl[i];
	return best;
}
static int cam_ctrl_get(unsigned id, int *val)
{
	int s = cam_conn(); if (s < 0) return -1;
	char req[48]; int n = snprintf(req, sizeof req, "CTRL_GET %u\n", id);
	if (uksd_wn(s, req, n) < 0) { cam_drop(); return -1; }
	char line[48]; if (uksd_rl(s, line, sizeof line) < 0) { cam_drop(); return -1; }
	return (sscanf(line, "OK %d", val) == 1) ? 0 : -1;
}
static int cam_ctrl_set(unsigned id, int val)
{
	int s = cam_conn(); if (s < 0) return -1;
	char req[64]; int n = snprintf(req, sizeof req, "CTRL_SET %u %d\n", id, val);
	if (uksd_wn(s, req, n) < 0) { cam_drop(); return -1; }
	char line[16]; if (uksd_rl(s, line, sizeof line) < 0) { cam_drop(); return -1; }
	return (line[0] == 'O' && line[1] == 'K') ? 0 : -1;
}
static int cam_menu(unsigned id, int idx, char *name, size_t nsz)
{
	int s = cam_conn(); if (s < 0) return -1;
	char req[48]; int n = snprintf(req, sizeof req, "CTRL_MENU %u\n", id);
	if (uksd_wn(s, req, n) < 0) { cam_drop(); return -1; }
	char line[96]; if (uksd_rl(s, line, sizeof line) < 0) { cam_drop(); return -1; }
	int cnt; if (sscanf(line, "OK %d", &cnt) != 1) return -1;
	int found = -1;
	for (int i = 0; i < cnt; i++) {
		if (uksd_rl(s, line, sizeof line) < 0) { cam_drop(); return -1; }
		int mi, consumed = 0;
		if (sscanf(line, "%d %n", &mi, &consumed) >= 1 && mi == idx) {
			snprintf(name, nsz, "%s", consumed > 0 ? line + consumed : "");
			found = 0;
		}
	}
	return found;
}

/* ---- per-fd capture state ---- */
#define CAM_MAXBUF 16
static struct cam_fd {
	int pid, fd, used;
	char tag[8]; unsigned fourcc; int w, h;        /* negotiated format */
	int sizeimage, bytesperline;
	int memory;                                    /* 0=none 1=MMAP 2=USERPTR */
	int nbuf; long long buflen;                    /* MMAP/USERPTR geometry */
	int filefd;                                    /* O_RDWR fd to the marker (MMAP pwrite) */
	unsigned long long uptr[CAM_MAXBUF];           /* USERPTR guest addrs */
	int qhead, qtail, qn; int q[CAM_MAXBUF];       /* queued-buffer FIFO */
	int streaming, started; unsigned seq;
} g_camfd[32];
static int g_ncamfd = 0;
/* Key capture state by TGID (process), not tracee->pid (a TID): libraries like
 * ffmpeg QBUF on one thread and DQBUF on another, sharing the same fd. */
static int cam_owner(Tracee *tracee) { return ukfs_tgid(tracee->pid); }
static struct cam_fd *camfd_get(int pid, int fd, int create)
{
	for (int i = 0; i < g_ncamfd; i++)
		if (g_camfd[i].used && g_camfd[i].pid == pid && g_camfd[i].fd == fd) return &g_camfd[i];
	if (!create) return NULL;
	int slot = -1;
	for (int i = 0; i < g_ncamfd; i++) if (!g_camfd[i].used) { slot = i; break; }
	if (slot < 0) { if (g_ncamfd >= 32) return NULL; slot = g_ncamfd++; }
	struct cam_fd *C = &g_camfd[slot];
	memset(C, 0, sizeof *C);
	C->used = 1; C->pid = pid; C->fd = fd; C->filefd = -1;
	C->fourcc = g_camfmt[0].fourcc;   /* caps already loaded by caller */
	snprintf(C->tag, sizeof C->tag, "%s", g_camfmt[0].tag);
	C->w = g_camfmt[0].nsz ? g_camfmt[0].sz[0].w : 640;
	C->h = g_camfmt[0].nsz ? g_camfmt[0].sz[0].h : 480;
	return C;
}
static void camfd_free(struct cam_fd *C)
{
	if (!C) return;
	if (C->filefd >= 0) close(C->filefd);
	C->used = 0;
}

/* Is fd a /dev/video* node? Match the bound marker host path. */
static int cam_is_fd(Tracee *tracee, int fd)
{
	char link[64], path[PATH_MAX];
	snprintf(link, sizeof link, "/proc/%d/fd/%d", tracee->pid, fd);
	ssize_t n = readlink(link, path, sizeof(path) - 1);
	if (n <= 0) return 0; path[n] = '\0';
	const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
	return (strncmp(b, "video", 5) == 0 && b[5] >= '0' && b[5] <= '9');
}
static int cam_host_path(Tracee *tracee, int fd, char *out, size_t osz)
{
	char link[64]; snprintf(link, sizeof link, "/proc/%d/fd/%d", tracee->pid, fd);
	ssize_t n = readlink(link, out, osz - 1);
	if (n <= 0) return -1; out[n] = '\0'; return 0;
}
/* basename is video<digit> (matches both the guest /dev/video0 and the bound
 * host marker .../sysdata/video0, since proot may have translated the path). */
static int cam_is_path(const char *p)
{
	const char *b = strrchr(p, '/'); b = b ? b + 1 : p;
	return strncmp(b, "video", 5) == 0 && b[5] >= '0' && b[5] <= '9' && b[6] == '\0';
}
/* Fake a char-special struct stat so v4l2 tools recognise /dev/video0 as a
 * video device (char, major 81). Native struct stat == the guest's kernel ABI
 * (proot runs same-arch guests), so this is correct on both host and device. */
static void cam_fill_stat(struct stat *st)
{
	memset(st, 0, sizeof *st);
	st->st_mode = S_IFCHR | 0660;
	st->st_nlink = 1;
	st->st_rdev = makedev(81, 0);
	st->st_blksize = 4096;
	st->st_ino = 0xca0e0;
}
/* statx is arch-independent; fill the basic stats for /dev/video0. */
static void cam_put_statx(Tracee *tracee, word_t addr)
{
	struct statx sx; memset(&sx, 0, sizeof sx);
	sx.stx_mask = STATX_BASIC_STATS;
	sx.stx_blksize = 4096;
	sx.stx_nlink = 1;
	sx.stx_mode = S_IFCHR | 0660;
	sx.stx_ino = 0xca0e0;
	sx.stx_rdev_major = 81; sx.stx_rdev_minor = 0;
	write_data(tracee, addr, &sx, sizeof sx);
}

static long long cam_pground(long long x) { return (x + 4095) & ~4095LL; }
static void cam_set_geom(struct cam_fd *C, unsigned fourcc, int w, int h)
{
	C->fourcc = fourcc; C->w = w; C->h = h;
	struct cam_fmt *F = cam_find_fmt(fourcc);
	snprintf(C->tag, sizeof C->tag, "%s", F ? F->tag : "MJPG");
	if (fourcc == V4L2_PIX_FMT_YUYV) { C->bytesperline = w * 2; C->sizeimage = w * h * 2; }
	else { C->bytesperline = 0; C->sizeimage = w * h * 2; }  /* MJPG: generous upper bound */
}

/* Pull a frame and deliver it for buffer index `idx`; returns bytesused or -1. */
static int cam_deliver(Tracee *tracee, struct cam_fd *C, int idx)
{
	unsigned char *fr; int fl;
	/* Block up to ~1s for a frame: real DQBUF blocks until one is ready, and the
	 * first frame after STREAMON can lag the camera spin-up. */
	if (cam_frame(1000, &fr, &fl) < 0) return -1;
	if (fl > (int)C->buflen) fl = (int)C->buflen;
	if (C->memory == V4L2_MEMORY_MMAP) {
		if (C->filefd < 0) return -1;
		if (pwrite(C->filefd, fr, (size_t)fl, (off_t)(idx * C->buflen)) != fl) return -1;
	} else if (C->memory == V4L2_MEMORY_USERPTR) {
		if (idx < 0 || idx >= CAM_MAXBUF || !C->uptr[idx]) return -1;
		if (write_data(tracee, (word_t)C->uptr[idx], fr, (word_t)fl) < 0) return -1;
	}
	return fl;
}

#define CAM_RET(v) do { poke_reg(tracee, SYSARG_RESULT, (word_t)(long)(v)); set_sysnum(tracee, PR_void); return true; } while (0)

static bool uknl_cam_dispatch(Tracee *tracee, word_t nr)
{
	if (!uk_cam_on()) return false;

	/* stat family: report /dev/video0 as a char device (major 81). The
	 * path-based forms are already trapped by proot for path translation. */
	if (nr == PR_newfstatat || nr == PR_fstatat64) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_2);
		char gp[PATH_MAX];
		if (pa && read_string(tracee, gp, pa, sizeof gp) > 0 && cam_is_path(gp)) {
			if (cam_caps() < 0) return false;
			struct stat st; cam_fill_stat(&st);
			write_data(tracee, peek_reg(tracee, CURRENT, SYSARG_3), &st, sizeof st);
			CAM_RET(0);
		}
		return false;
	}
	if (nr == PR_statx) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_2);
		char gp[PATH_MAX];
		if (pa && read_string(tracee, gp, pa, sizeof gp) > 0 && cam_is_path(gp)) {
			if (cam_caps() < 0) return false;
			cam_put_statx(tracee, peek_reg(tracee, CURRENT, SYSARG_5));
			CAM_RET(0);
		}
		return false;
	}
	if (nr == PR_fstat || nr == PR_fstat64) {
		int sfd = (int) peek_reg(tracee, CURRENT, SYSARG_1);
		if (cam_is_fd(tracee, sfd)) {
			struct stat st; cam_fill_stat(&st);
			write_data(tracee, peek_reg(tracee, CURRENT, SYSARG_2), &st, sizeof st);
			CAM_RET(0);
		}
		return false;
	}

	/* poll()/ppoll() on the video fd: a regular file is "always readable", so an
	 * unmodified poll lets apps (ffmpeg) busy-spin on DQBUF EAGAIN and never free
	 * buffers -> deadlock. Emulate V4L2 poll semantics: POLLIN only when a queued
	 * buffer can be filled (streaming && qn>0); otherwise sleep briefly and report
	 * a timeout so the app's loop runs and re-queues buffers. Only handle polls
	 * whose fds are all video fds (the common ffmpeg/OpenCV case). */
	if (nr == PR_poll || nr == PR_ppoll) {
		unsigned long nfds = (unsigned long) peek_reg(tracee, CURRENT, SYSARG_2);
		if (nfds == 0 || nfds > 8) return false;
		word_t fds_addr = peek_reg(tracee, CURRENT, SYSARG_1);
		struct pollfd pfds[8];
		if (read_data(tracee, pfds, fds_addr, nfds * sizeof(struct pollfd)) < 0) return false;
		int nvideo = 0;
		for (unsigned i = 0; i < nfds; i++) if (cam_is_fd(tracee, pfds[i].fd)) nvideo++;
		if (nvideo == 0 || (unsigned)nvideo != nfds) return false;  /* not (only) ours */
		int ready = 0;
		for (unsigned i = 0; i < nfds; i++) {
			struct cam_fd *C = camfd_get(cam_owner(tracee), pfds[i].fd, 0);
			short re = 0;
			if (C && C->streaming && C->qn > 0 && (pfds[i].events & POLLIN)) re |= POLLIN;
			pfds[i].revents = re; if (re) ready++;
		}
		if (!ready) { struct timespec ts = { 0, 15 * 1000 * 1000 }; nanosleep(&ts, NULL); }
		write_data(tracee, fds_addr, pfds, nfds * sizeof(struct pollfd));
		CAM_RET(ready);
	}

	if (nr != PR_ioctl && nr != PR_read && nr != PR_pread64 && nr != PR_close) return false;
	int fd = (int) peek_reg(tracee, CURRENT, SYSARG_1);

	if (nr == PR_close) {
		struct cam_fd *C = camfd_get(cam_owner(tracee), fd, 0);
		if (C) { if (C->streaming) cam_stop(); camfd_free(C); }
		return false;   /* let the real close run on the marker fd */
	}
	if (!cam_is_fd(tracee, fd)) return false;
	if (cam_caps() < 0) CAM_RET(-ENODEV);

	/* ---- read() I/O method: one frame per read ---- */
	if (nr == PR_read || nr == PR_pread64) {
		struct cam_fd *C = camfd_get(cam_owner(tracee), fd, 1);
		if (!C) CAM_RET(-ENOMEM);
		word_t buf = peek_reg(tracee, CURRENT, SYSARG_2);
		size_t len = (size_t) peek_reg(tracee, CURRENT, SYSARG_3);
		if (!C->started) { if (cam_start(C->tag, C->w, C->h) < 0) CAM_RET(-EIO); C->started = 1; C->streaming = 1; }
		unsigned char *fr; int fl;
		if (cam_frame(1000, &fr, &fl) < 0) CAM_RET(-EIO);
		if ((size_t)fl > len) fl = (int)len;
		if (fl > 0 && write_data(tracee, buf, fr, (word_t)fl) < 0) CAM_RET(-EFAULT);
		CAM_RET(fl);
	}

	/* ---- ioctl ---- */
	unsigned long cmd = (unsigned long) peek_reg(tracee, CURRENT, SYSARG_2);
	word_t arg = peek_reg(tracee, CURRENT, SYSARG_3);

	if (cmd == VIDIOC_QUERYCAP) {
		struct v4l2_capability cap; memset(&cap, 0, sizeof cap);
		snprintf((char *)cap.driver, sizeof cap.driver, "neoterm");
		snprintf((char *)cap.card, sizeof cap.card, "NeoTerm Camera");
		snprintf((char *)cap.bus_info, sizeof cap.bus_info, "platform:neoterm");
		cap.version = (5 << 16) | (10 << 8) | 0;   /* KERNEL_VERSION(5,10,0) */
		cap.capabilities = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
		                   V4L2_CAP_READWRITE | V4L2_CAP_DEVICE_CAPS;
		cap.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_READWRITE;
		write_data(tracee, arg, &cap, sizeof cap);
		CAM_RET(0);
	}
	if (cmd == VIDIOC_ENUM_FMT) {
		struct v4l2_fmtdesc fd_; if (read_data(tracee, &fd_, arg, sizeof fd_) < 0) CAM_RET(-EFAULT);
		if (fd_.type != V4L2_BUF_TYPE_VIDEO_CAPTURE) CAM_RET(-EINVAL);
		if ((int)fd_.index >= g_ncamfmt) CAM_RET(-EINVAL);
		struct cam_fmt *F = &g_camfmt[fd_.index];
		fd_.flags = (F->fourcc == V4L2_PIX_FMT_MJPEG) ? V4L2_FMT_FLAG_COMPRESSED : 0;
		fd_.pixelformat = F->fourcc;
		snprintf((char *)fd_.description, sizeof fd_.description, "%s", F->tag);
		write_data(tracee, arg, &fd_, sizeof fd_);
		CAM_RET(0);
	}
	if (cmd == VIDIOC_ENUM_FRAMESIZES) {
		struct v4l2_frmsizeenum fs; if (read_data(tracee, &fs, arg, sizeof fs) < 0) CAM_RET(-EFAULT);
		struct cam_fmt *F = cam_find_fmt(fs.pixel_format);
		if (!F || (int)fs.index >= F->nsz) CAM_RET(-EINVAL);
		fs.type = V4L2_FRMSIZE_TYPE_DISCRETE;
		fs.discrete.width = F->sz[fs.index].w;
		fs.discrete.height = F->sz[fs.index].h;
		write_data(tracee, arg, &fs, sizeof fs);
		CAM_RET(0);
	}
	if (cmd == VIDIOC_ENUM_FRAMEINTERVALS) {
		struct v4l2_frmivalenum fi; if (read_data(tracee, &fi, arg, sizeof fi) < 0) CAM_RET(-EFAULT);
		struct cam_fmt *F = cam_find_fmt(fi.pixel_format);
		if (!F || fi.index != 0) CAM_RET(-EINVAL);
		int fn = 30, fdv = 1;
		for (int i = 0; i < F->nsz; i++) if (F->sz[i].w == (int)fi.width && F->sz[i].h == (int)fi.height) { fn = F->sz[i].fn; fdv = F->sz[i].fd; }
		fi.type = V4L2_FRMIVAL_TYPE_DISCRETE;
		fi.discrete.numerator = fdv; fi.discrete.denominator = fn;
		write_data(tracee, arg, &fi, sizeof fi);
		CAM_RET(0);
	}
	if (cmd == VIDIOC_G_FMT) {
		struct v4l2_format f; if (read_data(tracee, &f, arg, sizeof f) < 0) CAM_RET(-EFAULT);
		if (f.type != V4L2_BUF_TYPE_VIDEO_CAPTURE) CAM_RET(-EINVAL);
		struct cam_fd *C = camfd_get(cam_owner(tracee), fd, 1); if (!C) CAM_RET(-ENOMEM);
		f.fmt.pix.width = C->w; f.fmt.pix.height = C->h; f.fmt.pix.pixelformat = C->fourcc;
		f.fmt.pix.field = V4L2_FIELD_NONE;
		f.fmt.pix.bytesperline = C->fourcc == V4L2_PIX_FMT_YUYV ? C->w * 2 : 0;
		f.fmt.pix.sizeimage = C->fourcc == V4L2_PIX_FMT_YUYV ? C->w * C->h * 2 : C->w * C->h * 2;
		f.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
		write_data(tracee, arg, &f, sizeof f);
		CAM_RET(0);
	}
	if (cmd == VIDIOC_S_FMT || cmd == VIDIOC_TRY_FMT) {
		struct v4l2_format f; if (read_data(tracee, &f, arg, sizeof f) < 0) CAM_RET(-EFAULT);
		if (f.type != V4L2_BUF_TYPE_VIDEO_CAPTURE) CAM_RET(-EINVAL);
		unsigned want = f.fmt.pix.pixelformat;
		struct cam_fmt *F = cam_find_fmt(want); if (!F) F = &g_camfmt[0];
		int w = f.fmt.pix.width, h = f.fmt.pix.height; cam_snap_size(F, &w, &h);
		f.fmt.pix.width = w; f.fmt.pix.height = h; f.fmt.pix.pixelformat = F->fourcc;
		f.fmt.pix.field = V4L2_FIELD_NONE;
		f.fmt.pix.bytesperline = F->fourcc == V4L2_PIX_FMT_YUYV ? w * 2 : 0;
		f.fmt.pix.sizeimage = w * h * 2;
		f.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
		if (cmd == VIDIOC_S_FMT) {
			struct cam_fd *C = camfd_get(cam_owner(tracee), fd, 1); if (!C) CAM_RET(-ENOMEM);
			cam_set_geom(C, F->fourcc, w, h);
		}
		write_data(tracee, arg, &f, sizeof f);
		CAM_RET(0);
	}
	if (cmd == VIDIOC_G_PARM || cmd == VIDIOC_S_PARM) {
		struct v4l2_streamparm p; if (read_data(tracee, &p, arg, sizeof p) < 0) CAM_RET(-EFAULT);
		if (p.type != V4L2_BUF_TYPE_VIDEO_CAPTURE) CAM_RET(-EINVAL);
		memset(&p.parm.capture, 0, sizeof p.parm.capture);
		p.parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
		p.parm.capture.timeperframe.numerator = 1;
		p.parm.capture.timeperframe.denominator = 30;
		p.parm.capture.readbuffers = 1;
		write_data(tracee, arg, &p, sizeof p);
		CAM_RET(0);
	}
	if (cmd == VIDIOC_REQBUFS) {
		struct v4l2_requestbuffers rb; if (read_data(tracee, &rb, arg, sizeof rb) < 0) CAM_RET(-EFAULT);
		if (rb.type != V4L2_BUF_TYPE_VIDEO_CAPTURE) CAM_RET(-EINVAL);
		if (rb.memory != V4L2_MEMORY_MMAP && rb.memory != V4L2_MEMORY_USERPTR) CAM_RET(-EINVAL);
		struct cam_fd *C = camfd_get(cam_owner(tracee), fd, 1); if (!C) CAM_RET(-ENOMEM);
		if (C->filefd >= 0) { close(C->filefd); C->filefd = -1; }
		C->qhead = C->qtail = C->qn = 0; C->streaming = 0;
		int cnt = (int) rb.count; if (cnt > CAM_MAXBUF) cnt = CAM_MAXBUF;
		if (cnt <= 0) { C->nbuf = 0; C->memory = 0; rb.count = 0; write_data(tracee, arg, &rb, sizeof rb); CAM_RET(0); }
		C->memory = rb.memory;
		C->buflen = cam_pground(C->sizeimage > 0 ? C->sizeimage : C->w * C->h * 2);
		C->nbuf = cnt;
		if (rb.memory == V4L2_MEMORY_MMAP) {
			char path[PATH_MAX];
			if (cam_host_path(tracee, fd, path, sizeof path) < 0) CAM_RET(-EIO);
			int ffd = open(path, O_RDWR);
			if (ffd < 0 || ftruncate(ffd, (off_t)(C->buflen * cnt)) < 0) { if (ffd >= 0) close(ffd); CAM_RET(-EIO); }
			C->filefd = ffd;
		}
		rb.count = cnt;
		write_data(tracee, arg, &rb, sizeof rb);
		CAM_RET(0);
	}
	if (cmd == VIDIOC_QUERYBUF) {
		struct v4l2_buffer b; if (read_data(tracee, &b, arg, sizeof b) < 0) CAM_RET(-EFAULT);
		struct cam_fd *C = camfd_get(cam_owner(tracee), fd, 0);
		if (!C || (int)b.index >= C->nbuf) CAM_RET(-EINVAL);
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = C->memory;
		b.length = (unsigned)C->buflen; b.bytesused = 0; b.flags = 0;
		if (C->memory == V4L2_MEMORY_MMAP) { b.m.offset = (unsigned)(b.index * C->buflen); b.flags = V4L2_BUF_FLAG_MAPPED; }
		write_data(tracee, arg, &b, sizeof b);
		CAM_RET(0);
	}
	if (cmd == VIDIOC_QBUF) {
		struct v4l2_buffer b; if (read_data(tracee, &b, arg, sizeof b) < 0) CAM_RET(-EFAULT);
		struct cam_fd *C = camfd_get(cam_owner(tracee), fd, 0);
		if (!C || (int)b.index >= C->nbuf || C->qn >= CAM_MAXBUF) CAM_RET(-EINVAL);
		if (C->memory == V4L2_MEMORY_USERPTR && b.index < CAM_MAXBUF) C->uptr[b.index] = b.m.userptr;
		C->q[C->qtail] = (int)b.index; C->qtail = (C->qtail + 1) % CAM_MAXBUF; C->qn++;
		b.flags = V4L2_BUF_FLAG_QUEUED;
		write_data(tracee, arg, &b, sizeof b);
		CAM_RET(0);
	}
	if (cmd == VIDIOC_DQBUF) {
		struct v4l2_buffer b; if (read_data(tracee, &b, arg, sizeof b) < 0) CAM_RET(-EFAULT);
		struct cam_fd *C = camfd_get(cam_owner(tracee), fd, 0);
		if (!C || C->qn <= 0) CAM_RET(-EAGAIN);
		int idx = C->q[C->qhead];
		int used = cam_deliver(tracee, C, idx);
		if (used < 0) CAM_RET(-EIO);
		C->qhead = (C->qhead + 1) % CAM_MAXBUF; C->qn--;
		struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
		memset(&b, 0, sizeof b);
		b.index = idx; b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = C->memory;
		b.bytesused = (unsigned)used; b.length = (unsigned)C->buflen;
		b.flags = V4L2_BUF_FLAG_DONE | V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
		if (C->memory == V4L2_MEMORY_MMAP) { b.m.offset = (unsigned)(idx * C->buflen); b.flags |= V4L2_BUF_FLAG_MAPPED; }
		else if (C->memory == V4L2_MEMORY_USERPTR && idx < CAM_MAXBUF) { b.m.userptr = C->uptr[idx]; }
		b.timestamp.tv_sec = ts.tv_sec; b.timestamp.tv_usec = ts.tv_nsec / 1000;
		b.sequence = C->seq++;
		write_data(tracee, arg, &b, sizeof b);
		CAM_RET(0);
	}
	if (cmd == VIDIOC_STREAMON) {
		struct cam_fd *C = camfd_get(cam_owner(tracee), fd, 1); if (!C) CAM_RET(-ENOMEM);
		if (cam_start(C->tag, C->w, C->h) < 0) CAM_RET(-EIO);
		C->streaming = 1; C->started = 1; C->seq = 0;
		CAM_RET(0);
	}
	if (cmd == VIDIOC_STREAMOFF) {
		struct cam_fd *C = camfd_get(cam_owner(tracee), fd, 0);
		if (C) { cam_stop(); C->streaming = 0; C->qhead = C->qtail = C->qn = 0; }
		CAM_RET(0);
	}
	if (cmd == VIDIOC_ENUMINPUT) {
		struct v4l2_input in; if (read_data(tracee, &in, arg, sizeof in) < 0) CAM_RET(-EFAULT);
		if (in.index != 0) CAM_RET(-EINVAL);
		memset(&in, 0, sizeof in); in.index = 0;
		snprintf((char *)in.name, sizeof in.name, "Camera");
		in.type = V4L2_INPUT_TYPE_CAMERA;
		write_data(tracee, arg, &in, sizeof in);
		CAM_RET(0);
	}
	if (cmd == VIDIOC_G_INPUT) { int z = 0; write_data(tracee, arg, &z, sizeof z); CAM_RET(0); }
	if (cmd == VIDIOC_S_INPUT) {
		int v = 0; read_data(tracee, &v, arg, sizeof v);
		if (v != 0) CAM_RET(-EINVAL);
		write_data(tracee, arg, &v, sizeof v); CAM_RET(0);
	}
	if (cmd == VIDIOC_QUERYCTRL) {
		struct v4l2_queryctrl q; if (read_data(tracee, &q, arg, sizeof q) < 0) CAM_RET(-EFAULT);
		struct cam_ctrl *C = (q.id & V4L2_CTRL_FLAG_NEXT_CTRL)
		    ? cam_ctrl_next(q.id & ~(V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND))
		    : cam_ctrl_find(q.id);
		if (!C) CAM_RET(-EINVAL);
		memset(&q, 0, sizeof q);
		q.id = C->id; q.type = C->type; q.minimum = C->min; q.maximum = C->max;
		q.step = C->step; q.default_value = C->def; q.flags = C->flags;
		snprintf((char *)q.name, sizeof q.name, "%s", C->name);
		write_data(tracee, arg, &q, sizeof q);
		CAM_RET(0);
	}
	if (cmd == VIDIOC_QUERYMENU) {
		struct v4l2_querymenu qm; if (read_data(tracee, &qm, arg, sizeof qm) < 0) CAM_RET(-EFAULT);
		char name[32] = {0};
		if (cam_menu(qm.id, (int)qm.index, name, sizeof name) < 0) CAM_RET(-EINVAL);
		snprintf((char *)qm.name, sizeof qm.name, "%s", name);
		write_data(tracee, arg, &qm, sizeof qm);
		CAM_RET(0);
	}
	if (cmd == VIDIOC_G_CTRL) {
		struct v4l2_control c; if (read_data(tracee, &c, arg, sizeof c) < 0) CAM_RET(-EFAULT);
		int v; if (cam_ctrl_get(c.id, &v) < 0) CAM_RET(-EINVAL);
		c.value = v; write_data(tracee, arg, &c, sizeof c); CAM_RET(0);
	}
	if (cmd == VIDIOC_S_CTRL) {
		struct v4l2_control c; if (read_data(tracee, &c, arg, sizeof c) < 0) CAM_RET(-EFAULT);
		if (cam_ctrl_set(c.id, c.value) < 0) CAM_RET(-EINVAL);
		write_data(tracee, arg, &c, sizeof c); CAM_RET(0);
	}
	if (cmd == VIDIOC_G_EXT_CTRLS || cmd == VIDIOC_S_EXT_CTRLS || cmd == VIDIOC_TRY_EXT_CTRLS) {
		struct v4l2_ext_controls ecs; if (read_data(tracee, &ecs, arg, sizeof ecs) < 0) CAM_RET(-EFAULT);
		word_t arr = (word_t) ecs.controls;
		int rc = 0;
		for (unsigned i = 0; i < ecs.count; i++) {
			struct v4l2_ext_control ec;
			word_t ea = arr + (word_t)(i * sizeof ec);
			if (read_data(tracee, &ec, ea, sizeof ec) < 0) { rc = -EFAULT; ecs.error_idx = i; break; }
			if (cmd == VIDIOC_G_EXT_CTRLS) {
				int v; if (cam_ctrl_get(ec.id, &v) < 0) { rc = -EINVAL; ecs.error_idx = i; break; }
				ec.value = v; write_data(tracee, ea, &ec, sizeof ec);
			} else {
				if ((cmd == VIDIOC_S_EXT_CTRLS) && cam_ctrl_set(ec.id, ec.value) < 0) { rc = -EINVAL; ecs.error_idx = i; break; }
			}
		}
		write_data(tracee, arg, &ecs, sizeof ecs);
		CAM_RET(rc);
	}
	if (cmd == VIDIOC_CROPCAP) {
		struct v4l2_cropcap cc; if (read_data(tracee, &cc, arg, sizeof cc) < 0) CAM_RET(-EFAULT);
		struct cam_fd *C = camfd_get(cam_owner(tracee), fd, 1); if (!C) CAM_RET(-ENOMEM);
		memset(&cc.bounds, 0, sizeof cc.bounds);
		cc.bounds.width = C->w; cc.bounds.height = C->h;
		cc.defrect = cc.bounds;
		cc.pixelaspect.numerator = 1; cc.pixelaspect.denominator = 1;
		write_data(tracee, arg, &cc, sizeof cc);
		CAM_RET(0);
	}

	/* Unknown ioctl: let it fall through to the real (regular-file) fd, which
	 * returns ENOTTY — exactly what an unsupported V4L2 ioctl should do. */
	return false;
}
#undef CAM_RET
