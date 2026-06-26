/* NeoTerm USB-filesystem redirect — injected verbatim into proot's
 * syscall/enter.c (just before translate_syscall_enter) by fakeid0-xattr.py.
 *
 * When the guest does mount("/dev/uksd0", "/mnt/usb", "vfat", ...), the real
 * Linux FS driver lives in the ukfsd daemon (io.neoterm.fs), not the host
 * kernel. proot can't issue a real mount, so instead it: (1) tells ukfsd to
 * MOUNT the block device, (2) records the guest mount point in a vmount table,
 * and (3) proxies every path syscall under that mount point to ukfsd, faking
 * each syscall result with set_sysnum(PR_void) + poke_reg(SYSARG_RESULT).
 *
 * This file is NOT compiled on its own (it has no includes / uses enter.c's
 * scope: Tracee, peek_reg/poke_reg, set_sysnum, read_string/write_data, the
 * PR_* sysnums, SYSARG_*, PATH_MAX, and the block proxy's uksd_wn/rn/rl socket
 * helpers, which are injected just above it). Task #2 scope: mount + client +
 * vmount table + stat. The open/read/getdents/write paths are added later.
 */

/* ---- gating: UK_FS=1 (set by ProotManager alongside UK_BLOCK) ---- */
static int g_uk_fs = -1;
static int uk_fs_on(void)
{
	if (g_uk_fs < 0) { const char *e = getenv("UK_FS"); g_uk_fs = (e && *e && *e != '0') ? 1 : 0; }
	return g_uk_fs;
}

/* TEMP debug: append a line to the app kmsg buffer (defined later, near uk_dbg). */
static void uk_dbg_line(const char *line);

/* ---- persistent io.neoterm.fs connection (ukfsd holds one mount per conn) ---- */
static int  g_ukfs_sock = -1;
static char g_vmount[PATH_MAX];      /* guest mount point, e.g. "/mnt/usb" */
static int  g_vmounted = 0;          /* guest issued mount here; cleared only on umount */
static int  g_ukfs_ready = 0;        /* 1 once ukfsd has actually mounted on g_ukfs_sock */

static int ukfs_conn(void)
{
	if (g_ukfs_sock >= 0) return g_ukfs_sock;
	int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (s < 0) return -1;
	struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX;
	const char *name = "io.neoterm.fs";
	a.sun_path[0] = '\0'; strncpy(a.sun_path + 1, name, sizeof(a.sun_path) - 2);
	socklen_t len = sizeof(a.sun_family) + 1 + strlen(name);
	if (connect(s, (struct sockaddr *) &a, len) < 0) {
		char l[80]; snprintf(l, sizeof l, "uk_fs: connect io.neoterm.fs FAILED errno=%d\n", errno);
		uk_dbg_line(l); close(s); return -1;
	}
	g_ukfs_sock = s;
	{ char l[64]; snprintf(l, sizeof l, "uk_fs: connected io.neoterm.fs fd=%d\n", s); uk_dbg_line(l); }
	return s;
}
/* Drop the io.neoterm.fs connection. The mount lives in the connection, so the
 * ukfsd-side mount is gone too -> clear g_ukfs_ready (next access re-mounts). Do
 * NOT touch g_vmounted: the guest still has /dev/uksd0 mounted at g_vmount, and
 * clearing it would disable ukfs_rel() and the dispatch guard, leaking every
 * path op under the mount point to the host (empty dir). Only a real guest
 * umount clears g_vmounted. */
static void ukfs_sdrop(void) { if (g_ukfs_sock >= 0) { close(g_ukfs_sock); g_ukfs_sock = -1; } g_ukfs_ready = 0; }

/* guest sees the block device at .../uksd0 (same convention as the block proxy) */
static int ukfs_src_is_dev(const char *p)
{
	size_t n = strlen(p);
	return n >= 6 && strcmp(p + n - 6, "/uksd0") == 0;
}

/* Map a guest path to its vmount-relative form ("/mnt/usb/a/b" -> "/a/b",
 * the mount point itself -> "/"). Returns 1 iff the path is under the vmount. */
static int ukfs_rel(const char *guest, char *out, size_t osz)
{
	if (!g_vmounted) return 0;
	size_t ml = strlen(g_vmount);
	if (strncmp(guest, g_vmount, ml) != 0) return 0;
	char c = guest[ml];
	if (c != '\0' && c != '/') return 0;            /* "/mnt/usb" vs "/mnt/usbX" */
	snprintf(out, osz, "%s", guest[ml] ? guest + ml : "/");
	return 1;
}

/* ukfsd is told to MOUNT lazily, on the first access under the vmount, NOT from
 * the mount(2) hook: on Android mount(2) is blocked by the parent seccomp and
 * handled via proot's SIGSYS path, and socket I/O from that context does not
 * deliver to ukfsd (the write is lost). The first path op under the mount point
 * (e.g. `ls`) runs on the normal syscall path, where socket I/O works.
 * (g_ukfs_ready is declared up top, next to g_vmounted.) */
static int ukfs_do_mount(void)
{
	ukfs_sdrop();   /* fresh connection for the mount */
	int s = ukfs_conn();
	if (s < 0) { uk_dbg_line("uk_fs: ukfs_conn FAILED (cannot reach io.neoterm.fs)\n"); return -1; }
	{
		/* NB: send the FULL command INCLUDING the trailing '\n' — ukfsd's
		 * read_line() blocks until it sees the newline. Use strlen (not a
		 * hard-coded length) so the byte count can never drift from the literal,
		 * and uksd_wn so partial writes / EINTR are handled. */
		static const char REQ[] = "MOUNT auto uksd0\n";
		int wn = uksd_wn(s, REQ, sizeof REQ - 1);
		if (wn < 0) {
			char l[96]; snprintf(l, sizeof l, "uk_fs: MOUNT write failed errno=%d\n", errno);
			uk_dbg_line(l); ukfs_sdrop(); return -1;
		}
	}
	char line[64];
	if (uksd_rl(s, line, sizeof line) < 0) { uk_dbg_line("uk_fs: MOUNT no reply\n"); ukfs_sdrop(); return -1; }
	if (line[0] == 'O' && line[1] == 'K') return 0;
	{ char l[128]; snprintf(l, sizeof l, "uk_fs: MOUNT rejected: '%s'\n", line); uk_dbg_line(l); }
	return -1;
}

/* Tell ukfsd to unmount and forget the vmount. Dropping the connection alone
 * already loses the ukfsd-side mount (one FS per connection), but send an
 * explicit UMOUNT first so ukfsd can flush/free before the close. */
static void ukfs_do_umount(void)
{
	if (g_ukfs_ready && g_ukfs_sock >= 0) {
		char line[32];
		if (uksd_wn(g_ukfs_sock, "UMOUNT\n", 7) >= 0)
			uksd_rl(g_ukfs_sock, line, sizeof line);   /* best-effort reply */
	}
	ukfs_sdrop();          /* closes the connection -> ukfsd drops the FS */
	g_vmounted = 0;
	g_vmount[0] = '\0';
}

/* True iff the io.neoterm.block server still has a device attached (SIZE -> OK).
 * Self-contained: a sibling uknl_block_present() exists in the block proxy, but
 * it is static in another translation unit, so we re-implement the probe here.
 * Returns 1 ("present") if the socket can't even be created (can't tell -> don't
 * auto-umount spuriously); 0 only when the server is reachable-but-deviceless or
 * unreachable (the pendrive is genuinely gone). */
static int ukfs_block_present(void)
{
	int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (s < 0) return 1;
	struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX;
	const char *nm = "io.neoterm.block";
	a.sun_path[0] = '\0'; strncpy(a.sun_path + 1, nm, sizeof(a.sun_path) - 2);
	socklen_t len = sizeof(a.sun_family) + 1 + strlen(nm);
	if (connect(s, (struct sockaddr *) &a, len) < 0) { close(s); return 0; }
	int ok = (write(s, "SIZE\n", 5) == 5);
	char r[8] = {0}; ssize_t n = ok ? read(s, r, sizeof r - 1) : -1;
	close(s);
	return n >= 2 && r[0] == 'O' && r[1] == 'K';
}

/* Perform the deferred ukfsd mount on first access (idempotent). A (re)mount is
 * needed whenever g_ukfs_ready is 0 — which also happens after a socket error
 * (ukfs_sdrop) when the pendrive is unplugged mid-session. At that point, if the
 * backing device is gone, auto-clear the vmount so the mount point reverts to the
 * empty host dir instead of erroring forever (the guest can't umount a device
 * that vanished). The block-present probe is only done when a mount is pending,
 * so it costs nothing on the steady-state mounted path. */
static void ukfs_ensure_mounted(void)
{
	if (!g_vmounted || g_ukfs_ready)
		return;
	if (!ukfs_block_present()) {
		uk_dbg_line("uk_fs: device gone -> auto-umount\n");
		ukfs_do_umount();
		return;
	}
	if (ukfs_do_mount() == 0) {
		g_ukfs_ready = 1;
		uk_dbg_line("uk_fs: lazy ukfsd mount OK\n");
	}
}

/* STAT one path. Returns 0 (filled), -2 (ENOENT from ukfsd), -1 (socket error). */
static int ukfs_query_stat(const char *rel, unsigned *mode, unsigned *uid, unsigned *gid,
	long *size, unsigned long *ino, long *mtime, long *atime, unsigned *nlink,
	unsigned long *rdev, unsigned long *blocks)
{
	int s = ukfs_conn(); if (s < 0) return -1;
	char req[PATH_MAX + 16];
	int n = snprintf(req, sizeof req, "STAT %s\n", rel);
	if (uksd_wn(s, req, n) < 0) { ukfs_sdrop(); return -1; }
	char line[256];
	if (uksd_rl(s, line, sizeof line) < 0) { ukfs_sdrop(); return -1; }
	if (sscanf(line, "OK %u %u %u %ld %lu %ld %ld %u %lu %lu",
	           mode, uid, gid, size, ino, mtime, atime, nlink, rdev, blocks) != 10)
		return -2;
	return 0;
}

/* aarch64 struct stat (128 B) — offsets match the block proxy's uksd_put_stat,
 * plus uid/gid (24/28) and a/m/ctime seconds (72/88/104). */
static void ukfs_put_stat(Tracee *tracee, word_t addr, unsigned mode, unsigned uid, unsigned gid,
	long size, unsigned long ino, long mtime, long atime, unsigned nlink,
	unsigned long rdev, unsigned long blocks)
{
	unsigned char st[128]; memset(st, 0, sizeof st);
	unsigned long st_ino = ino, st_rdev = rdev;
	unsigned int  st_mode = mode, st_nlink = nlink ? nlink : 1, st_uid = uid, st_gid = gid;
	long st_size = size, st_blocks = (long) blocks; int st_blksize = 512;
	long at_s = atime, mt_s = mtime, ct_s = mtime;
	memcpy(st + 8,  &st_ino,     8);
	memcpy(st + 16, &st_mode,    4);
	memcpy(st + 20, &st_nlink,   4);
	memcpy(st + 24, &st_uid,     4);
	memcpy(st + 28, &st_gid,     4);
	memcpy(st + 32, &st_rdev,    8);
	memcpy(st + 48, &st_size,    8);
	memcpy(st + 56, &st_blksize, 4);
	memcpy(st + 64, &st_blocks,  8);
	memcpy(st + 72, &at_s,       8);   /* st_atime.tv_sec  */
	memcpy(st + 88, &mt_s,       8);   /* st_mtime.tv_sec  */
	memcpy(st + 104, &ct_s,      8);   /* st_ctime.tv_sec  */
	write_data(tracee, addr, st, sizeof st);
}

/* struct statx (256 B) — kernel uapi layout. */
static void ukfs_put_statx(Tracee *tracee, word_t addr, unsigned mode, unsigned uid, unsigned gid,
	long size, unsigned long ino, long mtime, long atime, unsigned nlink,
	unsigned long rdev, unsigned long blocks)
{
	unsigned char sx[256]; memset(sx, 0, sizeof sx);
	unsigned int  stx_mask = 0x000007ffU;   /* STATX_BASIC_STATS */
	unsigned int  stx_blksize = 512, stx_nlink = nlink ? nlink : 1, stx_uid = uid, stx_gid = gid;
	unsigned short stx_mode = (unsigned short) mode;
	unsigned long long stx_ino = ino, stx_size = (unsigned long long) size,
	                   stx_blocks = (unsigned long long) blocks;
	long long at_s = atime, ct_s = mtime, mt_s = mtime;
	unsigned int rdev_major = (unsigned)((rdev >> 8) & 0xfff), rdev_minor = (unsigned)(rdev & 0xff);
	memcpy(sx + 0,   &stx_mask,    4);
	memcpy(sx + 4,   &stx_blksize, 4);
	memcpy(sx + 16,  &stx_nlink,   4);
	memcpy(sx + 20,  &stx_uid,     4);
	memcpy(sx + 24,  &stx_gid,     4);
	memcpy(sx + 28,  &stx_mode,    2);
	memcpy(sx + 32,  &stx_ino,     8);
	memcpy(sx + 40,  &stx_size,    8);
	memcpy(sx + 48,  &stx_blocks,  8);
	memcpy(sx + 64,  &at_s,        8);   /* stx_atime.tv_sec */
	memcpy(sx + 96,  &ct_s,        8);   /* stx_ctime.tv_sec */
	memcpy(sx + 112, &mt_s,        8);   /* stx_mtime.tv_sec */
	memcpy(sx + 128, &rdev_major,  4);
	memcpy(sx + 132, &rdev_minor,  4);
	write_data(tracee, addr, sx, sizeof sx);
}

/* ---- ukfsd data ops used by the open/read/getdents machinery ---- */

/* READ <off> <len> <path>: bytes into buf (<= len), or -1 on error. */
static long ukfs_read_at(const char *rel, long long off, void *buf, size_t len)
{
	int s = ukfs_conn(); if (s < 0) return -1;
	char req[PATH_MAX + 48];
	int rl = snprintf(req, sizeof req, "READ %lld %zu %s\n", off, len, rel);
	if (uksd_wn(s, req, rl) < 0) { ukfs_sdrop(); return -1; }
	char line[64];
	if (uksd_rl(s, line, sizeof line) < 0) { ukfs_sdrop(); return -1; }
	long n; if (sscanf(line, "OK %ld", &n) != 1) return -1;
	if (n < 0) return -1;
	if ((size_t) n > len) n = (long) len;
	if (n > 0 && uksd_rn(s, buf, (size_t) n) < 0) { ukfs_sdrop(); return -1; }
	return n;
}

/* READLINK <path>: target into buf, length or -1. */
static long ukfs_readlink_at(const char *rel, char *buf, size_t bufsz)
{
	int s = ukfs_conn(); if (s < 0) return -1;
	char req[PATH_MAX + 16];
	int rl = snprintf(req, sizeof req, "READLINK %s\n", rel);
	if (uksd_wn(s, req, rl) < 0) { ukfs_sdrop(); return -1; }
	char line[64];
	if (uksd_rl(s, line, sizeof line) < 0) { ukfs_sdrop(); return -1; }
	long n; if (sscanf(line, "OK %ld", &n) != 1) return -1;
	if (n < 0) return -1;
	if ((size_t) n > bufsz) n = (long) bufsz;
	if (n > 0 && uksd_rn(s, buf, (size_t) n) < 0) { ukfs_sdrop(); return -1; }
	return n;
}

/* CREATE <mode> <path>: 0 ok / -1. */
static int ukfs_create_at(const char *rel, unsigned mode)
{
	int s = ukfs_conn(); if (s < 0) return -1;
	char req[PATH_MAX + 32];
	int rl = snprintf(req, sizeof req, "CREATE %u %s\n", mode, rel);
	if (uksd_wn(s, req, rl) < 0) { ukfs_sdrop(); return -1; }
	char line[32]; if (uksd_rl(s, line, sizeof line) < 0) { ukfs_sdrop(); return -1; }
	return (line[0] == 'O' && line[1] == 'K') ? 0 : -1;
}

/* WRITE <off> <len> <path> + payload: bytes written, or -1. */
static long ukfs_write_at(const char *rel, long long off, const void *buf, size_t len)
{
	int s = ukfs_conn(); if (s < 0) return -1;
	char req[PATH_MAX + 48];
	int rl = snprintf(req, sizeof req, "WRITE %lld %zu %s\n", off, len, rel);
	if (uksd_wn(s, req, rl) < 0 || uksd_wn(s, buf, len) < 0) { ukfs_sdrop(); return -1; }
	char line[64]; if (uksd_rl(s, line, sizeof line) < 0) { ukfs_sdrop(); return -1; }
	long n; if (sscanf(line, "OK %ld", &n) != 1) return -1;
	return n;
}

/* "<prefix><path>" ops (prefix carries the command + any leading numeric args
 * and a trailing space, e.g. "MKDIR 511 "). Returns 0, or -errno from ukfsd. */
static int ukfs_simple(const char *prefix, const char *rel)
{
	int s = ukfs_conn(); if (s < 0) return -1;
	char req[PATH_MAX + 64];
	int rl = snprintf(req, sizeof req, "%s%s\n", prefix, rel);
	if (uksd_wn(s, req, rl) < 0) { ukfs_sdrop(); return -1; }
	char line[64]; if (uksd_rl(s, line, sizeof line) < 0) { ukfs_sdrop(); return -1; }
	if (line[0] == 'O' && line[1] == 'K') return 0;
	int e; return (sscanf(line, "ERR %d", &e) == 1) ? -e : -1;
}

/* Two byte-counted paths in the payload (RENAME old/new, SYMLINK target/link). */
static int ukfs_two_path(const char *cmd, const char *a, const char *b)
{
	int s = ukfs_conn(); if (s < 0) return -1;
	size_t al = strlen(a), bl = strlen(b);
	char req[64]; int rl = snprintf(req, sizeof req, "%s %zu %zu\n", cmd, al, bl);
	if (uksd_wn(s, req, rl) < 0 || uksd_wn(s, a, al) < 0 || uksd_wn(s, b, bl) < 0) { ukfs_sdrop(); return -1; }
	char line[64]; if (uksd_rl(s, line, sizeof line) < 0) { ukfs_sdrop(); return -1; }
	if (line[0] == 'O' && line[1] == 'K') return 0;
	int e; return (sscanf(line, "ERR %d", &e) == 1) ? -e : -1;
}

/* ---- vfd table: a guest fd (over a placeholder file) -> a ukfsd path. The
 *      guest opens a real placeholder (/dev/null or /), gets a real fd, and
 *      every read/lseek/getdents/close on it is proxied here. ---- */
struct ukfs_dent { unsigned long long ino; int type; char name[256]; };
struct ukfs_vfd {
	int used, pid, fd, isdir;
	long long off;                  /* regular-file read cursor */
	char path[PATH_MAX];            /* vmount-relative */
	struct ukfs_dent *dents;        /* dir: parsed LIST incl. "." and ".." */
	int dent_n, dent_idx;           /* count and emit cursor */
};
struct ukfs_pending { int used, pid, isdir; char path[PATH_MAX]; };
static struct ukfs_vfd     g_vfd[128];
static struct ukfs_pending g_open_pending[64];

static struct ukfs_vfd *vfd_find(int pid, int fd)
{
	for (int i = 0; i < 128; i++)
		if (g_vfd[i].used && g_vfd[i].pid == pid && g_vfd[i].fd == fd) return &g_vfd[i];
	return NULL;
}
static struct ukfs_vfd *vfd_alloc(void)
{
	for (int i = 0; i < 128; i++) if (!g_vfd[i].used) return &g_vfd[i];
	return NULL;
}
static void vfd_free(struct ukfs_vfd *v) { if (v) { free(v->dents); memset(v, 0, sizeof *v); } }

static void open_pending_set(int pid, int isdir, const char *path)
{
	int i;
	for (i = 0; i < 64; i++) if (g_open_pending[i].used && g_open_pending[i].pid == pid) break;
	if (i == 64) for (i = 0; i < 64; i++) if (!g_open_pending[i].used) break;
	if (i == 64) return;
	g_open_pending[i].used = 1; g_open_pending[i].pid = pid; g_open_pending[i].isdir = isdir;
	snprintf(g_open_pending[i].path, sizeof g_open_pending[i].path, "%s", path);
}

/* Lazily LIST the directory and parse it (with synthetic "." / "..") on the
 * first getdents64. blob format: count x {u8 type, u64 ino, u64 size, u16
 * namelen, name[]} (little-endian), matching ukfsd's LIST reply. */
static int ukfs_load_dir(struct ukfs_vfd *v)
{
	int s = ukfs_conn(); if (s < 0) return -1;
	char req[PATH_MAX + 16];
	int rl = snprintf(req, sizeof req, "LIST %s\n", v->path);
	if (uksd_wn(s, req, rl) < 0) { ukfs_sdrop(); return -1; }
	char line[64];
	if (uksd_rl(s, line, sizeof line) < 0) { ukfs_sdrop(); return -1; }
	int cnt; size_t bytes;
	if (sscanf(line, "OK %d %zu", &cnt, &bytes) != 2 || cnt < 0) return -1;
	unsigned char *blob = NULL;
	if (bytes) {
		blob = malloc(bytes); if (!blob) return -1;
		if (uksd_rn(s, blob, bytes) < 0) { free(blob); ukfs_sdrop(); return -1; }
	}
	v->dents = calloc((size_t) cnt + 2, sizeof(struct ukfs_dent));
	if (!v->dents) { free(blob); return -1; }
	v->dents[0].type = 1; strcpy(v->dents[0].name, ".");
	v->dents[1].type = 1; strcpy(v->dents[1].name, "..");
	int k = 2; size_t o = 0;
	for (int i = 0; i < cnt && o + 19 <= bytes; i++) {
		unsigned char t = blob[o];
		unsigned long long ino; memcpy(&ino, blob + o + 1, 8);
		unsigned short nl;      memcpy(&nl, blob + o + 17, 2);
		o += 19;
		if (o + nl > bytes) break;
		size_t cn = nl < 255 ? nl : 255;
		memcpy(v->dents[k].name, blob + o, cn); v->dents[k].name[cn] = '\0';
		v->dents[k].ino = ino; v->dents[k].type = t;
		o += nl; k++;
	}
	v->dent_n = k; v->dent_idx = 0;
	free(blob);
	return 0;
}

/* Emit linux_dirent64 records into out[0..cap) from v->dents starting at the
 * emit cursor; advance it. Returns bytes written (0 = nothing fit / EOF).
 * Record layout: d_ino(8) d_off(8) d_reclen(2) d_type(1) d_name(NUL-term), the
 * whole record padded to 8 bytes. */
static size_t ukfs_emit_dents(struct ukfs_vfd *v, unsigned char *out, size_t cap)
{
	size_t used = 0;
	while (v->dent_idx < v->dent_n) {
		struct ukfs_dent *d = &v->dents[v->dent_idx];
		size_t nl = strlen(d->name);
		size_t reclen = (19 + nl + 1 + 7) & ~((size_t) 7);
		if (used + reclen > cap) break;
		unsigned char *p = out + used;
		memset(p, 0, reclen);
		unsigned long long d_ino = d->ino ? d->ino : (unsigned long long)(v->dent_idx + 1);
		long long d_off = (long long)(v->dent_idx + 1);     /* opaque resume cookie */
		unsigned short d_reclen = (unsigned short) reclen;
		unsigned char d_type = (d->type == 1) ? 4 /*DT_DIR*/ : (d->type == 2) ? 8 /*DT_REG*/ : 0;
		memcpy(p + 0, &d_ino, 8);
		memcpy(p + 8, &d_off, 8);
		memcpy(p + 16, &d_reclen, 2);
		p[18] = d_type;
		memcpy(p + 19, d->name, nl);
		used += reclen;
		v->dent_idx++;
	}
	return used;
}

/* Entry point, called from translate_syscall_enter before the syscall switch.
 * Returns true if the syscall was fully emulated (PR_void'd + result poked).
 * For openat it returns false after rewriting the path, so the (placeholder)
 * open proceeds and the fd is captured in uknl_fs_open_exit. */
/* --- TEMP DEBUG (remove after diagnosis): append a line to the app's kmsg
 * buffer (the host file bound at the guest /dev/kmsg), so it shows up in
 * `dmesg`. The host path is resolved once (from whichever tracee calls first)
 * and cached, so it also works for child tracees (e.g. the `mount` process)
 * whose binding lookup may differ. --- */
static char g_uk_kmsg[PATH_MAX];
static int  g_uk_kmsg_done;
static void uk_dbg_line(const char *line)
{
	if (!g_uk_kmsg[0]) return;
	int fd = open(g_uk_kmsg, O_WRONLY | O_APPEND | O_CLOEXEC);
	if (fd >= 0) { (void) write(fd, line, strlen(line)); close(fd); }
}
static void uk_dbg(Tracee *tracee, const char *line)
{
	if (!g_uk_kmsg_done) {
		g_uk_kmsg_done = 1;
		char kp[PATH_MAX]; strcpy(kp, "/dev/kmsg");
		Binding *b = get_binding(tracee, GUEST, kp);
		if (b) strncpy(g_uk_kmsg, b->host.path, sizeof g_uk_kmsg - 1);
	}
	uk_dbg_line(line);
}

/* Mount hook, called from apply_emulated_mount() — the common point for BOTH
 * the normal mount(2) trap AND the SIGSYS path Android uses to block mount(2)
 * (which bypasses translate_syscall_enter, hence the redirect's own PR_mount
 * branch never runs on-device). Returns true if it handled a /dev/uksd0 mount:
 * tells ukfsd to MOUNT and records the vmount; the caller then reports success
 * to the guest. */
static bool uknl_fs_mount_hook(Tracee *tracee)
{
	if (!uk_fs_on()) return false;
	char src[PATH_MAX];
	if (get_sysarg_path(tracee, src, SYSARG_1) < 0) return false;
	if (!ukfs_src_is_dev(src)) return false;
	char tgt[PATH_MAX];
	if (get_sysarg_path(tracee, tgt, SYSARG_2) < 0) return false;
	/* Register the vmount only — no socket I/O here (this may run in the SIGSYS
	 * context where it wouldn't deliver). The ukfsd MOUNT happens lazily on the
	 * first access under the mount point. */
	snprintf(g_vmount, sizeof g_vmount, "%s", tgt);
	g_vmounted = 1;
	g_ukfs_ready = 0;
	char l[PATH_MAX + 96];
	snprintf(l, sizeof l, "uk_fs: MOUNT hook src='%s' tgt='%s' (deferred)\n", src, tgt);
	uk_dbg(tracee, l);
	return true;
}

/* Umount hook, called from apply_emulated_umount() — the SIGSYS path Android
 * uses to block umount(2)/umount2(2). Returns true if it handled an unmount of
 * our vmount point (the caller then reports success to the guest). */
static bool uknl_fs_umount_hook(Tracee *tracee)
{
	if (!uk_fs_on() || !g_vmounted) return false;
	char tgt[PATH_MAX];
	if (get_sysarg_path(tracee, tgt, SYSARG_1) < 0) return false;
	if (strcmp(tgt, g_vmount) != 0) return false;   /* not our mount point */
	char l[PATH_MAX + 64];
	snprintf(l, sizeof l, "uk_fs: UMOUNT hook tgt='%s'\n", tgt);
	uk_dbg(tracee, l);
	ukfs_do_umount();
	return true;
}

#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif
/* Resolve an *at (dirfd, user path) pair to a vmount-relative path. Absolute
 * paths go through ukfs_rel; an AT_FDCWD-relative path is joined onto the guest
 * CWD (so `cd /mnt/usb2; ls` / `cat file` work); any other relative path is
 * joined onto the dirfd's vfd path (cp, ls -l, find, …). Returns 1 (rel filled)
 * iff the target is under the vmount. */
static int ukfs_rel_at(Tracee *tracee, int dirfd, const char *path, char *rel, size_t rsz)
{
	if (path[0] == '/')
		return ukfs_rel(path, rel, rsz);
	if (dirfd == AT_FDCWD) {
		/* Join the guest CWD; ukfs_rel then checks it's under the vmount. */
		const char *cwd = (tracee->fs && tracee->fs->cwd) ? tracee->fs->cwd : "/";
		char abs[PATH_MAX];
		snprintf(abs, sizeof abs, "%s/%s", cwd, path);
		return ukfs_rel(abs, rel, rsz);
	}
	struct ukfs_vfd *v = vfd_find(tracee->pid, dirfd);
	if (!v) return 0;                       /* dirfd isn't one of our mount fds */
	if (!path[0])
		snprintf(rel, rsz, "%s", v->path);              /* empty path => the dir itself */
	else if (v->path[0] == '/' && v->path[1] == '\0')
		snprintf(rel, rsz, "/%s", path);                /* dir is the mount root */
	else
		snprintf(rel, rsz, "%s/%s", v->path, path);
	return 1;
}

/* Shared openat/openat2 redirect for a vmount-relative path @rel with open
 * @flags. Returns 1 if fully handled (SYSARG_RESULT poked + PR_void), 0 if it
 * rewrote the path to a placeholder so the real open proceeds (fd bound at exit),
 * -1 to not intercept (socket down -> let the host try). */
static int ukfs_open_redirect(Tracee *tracee, const char *rel, int flags)
{
	unsigned mode, uid, gid, nlink; long size, mt, at; unsigned long ino, rdev, blocks;
	int q = ukfs_query_stat(rel, &mode, &uid, &gid, &size, &ino, &mt, &at, &nlink, &rdev, &blocks);
	if (q == -1) return -1;
	int isdir = (q == 0) && ((mode & 0170000) == 0040000);   /* S_IFDIR */
	if (q == -2) {
		if (flags & O_CREAT) {
			if (ukfs_create_at(rel, 0100644) < 0) {
				poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - EIO); set_sysnum(tracee, PR_void); return 1;
			}
			isdir = 0;
		} else {
			poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - ENOENT); set_sysnum(tracee, PR_void); return 1;
		}
	} else if ((flags & O_CREAT) && (flags & O_EXCL)) {
		/* O_CREAT|O_EXCL on an existing file -> EEXIST (proper semantics). */
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - EEXIST); set_sysnum(tracee, PR_void); return 1;
	}
	if ((flags & O_DIRECTORY) && !isdir) {
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - ENOTDIR); set_sysnum(tracee, PR_void); return 1;
	}
	/* O_TRUNC on an existing regular file: truncate the ukfs file now (the
	 * /dev/null placeholder open won't), else `echo > existing` leaves stale
	 * tail bytes past the freshly written content. q==0 => the file existed. */
	if (!isdir && q == 0 && (flags & O_TRUNC))
		(void) ukfs_simple("TRUNCATE 0 ", rel);
	(void) set_sysarg_path(tracee, isdir ? "/" : "/dev/null", SYSARG_2);
	open_pending_set(tracee->pid, isdir, rel);
	return 0;
}

static bool uknl_fs_dispatch(Tracee *tracee, word_t nr)
{
	/* --- TEMP DEBUG v3: one-time INIT line at the first syscall (shows the raw
	 * env proot actually sees), plus a per-mount(2) trace. Both go to the app's
	 * kmsg buffer -> visible in `dmesg | grep uk_fs`. --- */
	static int uk_dbg_init = 0;
	if (!uk_dbg_init) {
		uk_dbg_init = 1;
		char l[256];
		snprintf(l, sizeof l, "uk_fs: INIT v16-fchmod UK_FS='%s' UK_BLOCK='%s'\n",
		         getenv("UK_FS") ? getenv("UK_FS") : "(null)",
		         getenv("UK_BLOCK") ? getenv("UK_BLOCK") : "(null)");
		uk_dbg(tracee, l);
	}
	if (nr == PR_mount) {
		word_t dsa = peek_reg(tracee, CURRENT, SYSARG_1);
		char dbg[PATH_MAX] = {0};
		if (dsa) read_string(tracee, dbg, dsa, sizeof dbg);
		char line[PATH_MAX + 160];
		snprintf(line, sizeof line,
		         "uk_fs: PR_mount src='%s' on=%d is_dev=%d\n",
		         dbg, uk_fs_on(), ukfs_src_is_dev(dbg));
		uk_dbg(tracee, line);
	}

	if (!uk_fs_on()) return false;

	/* mount(2) on the NORMAL path (non-Android, where mount isn't SIGSYS-blocked):
	 * register the vmount and fake success. The actual ukfsd MOUNT is deferred to
	 * first access, same as the apply_emulated_mount hook used on Android. */
	if (nr == PR_mount) {
		word_t sa = peek_reg(tracee, CURRENT, SYSARG_1);
		char src[PATH_MAX];
		if (!sa || read_string(tracee, src, sa, sizeof src) <= 0) return false;
		if (!ukfs_src_is_dev(src)) return false;
		word_t ta = peek_reg(tracee, CURRENT, SYSARG_2);
		char tgt[PATH_MAX];
		if (!ta || read_string(tracee, tgt, ta, sizeof tgt) <= 0) return false;
		snprintf(g_vmount, sizeof g_vmount, "%s", tgt);
		g_vmounted = 1; g_ukfs_ready = 0;
		poke_reg(tracee, SYSARG_RESULT, 0);
		set_sysnum(tracee, PR_void);
		return true;
	}

	/* umount(2)/umount2(2) on the NORMAL path (non-Android). On Android these are
	 * SIGSYS-blocked and handled via the apply_emulated_umount hook instead. */
	if (nr == PR_umount2) {
		if (!g_vmounted) return false;
		word_t ta = peek_reg(tracee, CURRENT, SYSARG_1);
		char tgt[PATH_MAX];
		if (!ta || read_string(tracee, tgt, ta, sizeof tgt) <= 0) return false;
		if (strcmp(tgt, g_vmount) != 0) return false;
		ukfs_do_umount();
		poke_reg(tracee, SYSARG_RESULT, 0);
		set_sysnum(tracee, PR_void);
		return true;
	}

	if (!g_vmounted) return false;
	ukfs_ensure_mounted();   /* perform the deferred ukfsd MOUNT on first access */

	/* fd-based ops on a vfd (set up by a prior openat). */
	if (nr == PR_read || nr == PR_pread64) {
		struct ukfs_vfd *v = vfd_find(tracee->pid, (int) peek_reg(tracee, CURRENT, SYSARG_1));
		if (!v || v->isdir) return false;
		long long pos = (nr == PR_pread64)
			? (long long)(off_t) peek_reg(tracee, CURRENT, SYSARG_4) : v->off;
		size_t len = (size_t) peek_reg(tracee, CURRENT, SYSARG_3);
		word_t buf = peek_reg(tracee, CURRENT, SYSARG_2);
		if (len == 0) { poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true; }
		if (len > (8u << 20)) len = 8u << 20;
		void *tmp = malloc(len); if (!tmp) return false;
		long n = ukfs_read_at(v->path, pos, tmp, len);
		if (n > 0) write_data(tracee, buf, tmp, (size_t) n);
		free(tmp);
		if (n < 0) { poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - EIO); set_sysnum(tracee, PR_void); return true; }
		if (nr == PR_read) v->off += n;
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) n); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_write || nr == PR_pwrite64) {
		struct ukfs_vfd *v = vfd_find(tracee->pid, (int) peek_reg(tracee, CURRENT, SYSARG_1));
		if (!v || v->isdir) return false;
		long long pos = (nr == PR_pwrite64)
			? (long long)(off_t) peek_reg(tracee, CURRENT, SYSARG_4) : v->off;
		size_t len = (size_t) peek_reg(tracee, CURRENT, SYSARG_3);
		word_t buf = peek_reg(tracee, CURRENT, SYSARG_2);
		if (len == 0) { poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true; }
		if (len > (8u << 20)) len = 8u << 20;
		void *tmp = malloc(len); if (!tmp) return false;
		if (read_data(tracee, tmp, buf, len) < 0) {
			free(tmp); poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - EFAULT); set_sysnum(tracee, PR_void); return true;
		}
		long n = ukfs_write_at(v->path, pos, tmp, len);
		free(tmp);
		if (n < 0) { poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - EIO); set_sysnum(tracee, PR_void); return true; }
		if (nr == PR_write) v->off += n;
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) n); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_ftruncate) {
		struct ukfs_vfd *v = vfd_find(tracee->pid, (int) peek_reg(tracee, CURRENT, SYSARG_1));
		if (!v) return false;
		long long sz = (long long)(off_t) peek_reg(tracee, CURRENT, SYSARG_2);
		char pfx[48]; snprintf(pfx, sizeof pfx, "TRUNCATE %lld ", sz);
		int r = ukfs_simple(pfx, v->path);
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) r); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_lseek) {
		struct ukfs_vfd *v = vfd_find(tracee->pid, (int) peek_reg(tracee, CURRENT, SYSARG_1));
		if (!v) return false;
		long long off = (long long)(off_t) peek_reg(tracee, CURRENT, SYSARG_2);
		int whence = (int) peek_reg(tracee, CURRENT, SYSARG_3);
		long long base = 0;
		if (whence == 1) base = v->off;                 /* SEEK_CUR */
		else if (whence == 2) {                          /* SEEK_END */
			unsigned mode, uid, gid, nlink; long size, mt, at; unsigned long ino, rdev, blocks;
			if (ukfs_query_stat(v->path, &mode, &uid, &gid, &size, &ino, &mt, &at, &nlink, &rdev, &blocks) == 0)
				base = size;
		}
		long long no = base + off;
		if (no < 0) { poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - EINVAL); set_sysnum(tracee, PR_void); return true; }
		v->off = no;
		poke_reg(tracee, SYSARG_RESULT, (word_t) no); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_getdents64) {
		struct ukfs_vfd *v = vfd_find(tracee->pid, (int) peek_reg(tracee, CURRENT, SYSARG_1));
		if (!v || !v->isdir) return false;
		if (!v->dents && ukfs_load_dir(v) < 0) {
			poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - EIO); set_sysnum(tracee, PR_void); return true;
		}
		word_t buf = peek_reg(tracee, CURRENT, SYSARG_2);
		size_t cap = (size_t) peek_reg(tracee, CURRENT, SYSARG_3);
		if (cap > (1u << 20)) cap = 1u << 20;
		unsigned char *tmp = malloc(cap ? cap : 1); if (!tmp) return false;
		size_t w = ukfs_emit_dents(v, tmp, cap);
		if (w == 0 && v->dent_idx < v->dent_n) {        /* buffer too small for one record */
			free(tmp); poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - EINVAL); set_sysnum(tracee, PR_void); return true;
		}
		if (w) write_data(tracee, buf, tmp, w);
		free(tmp);
		poke_reg(tracee, SYSARG_RESULT, (word_t) w); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_close) {
		struct ukfs_vfd *v = vfd_find(tracee->pid, (int) peek_reg(tracee, CURRENT, SYSARG_1));
		if (v) vfd_free(v);
		return false;          /* let the real close of the placeholder fd run */
	}
	/* fsync/fdatasync on a vfd: ukfs writes are already flushed (uk_flush_meta),
	 * and the placeholder fd is /dev/null whose fsync returns EINVAL — which makes
	 * editors report "Error writing ...: Invalid argument". Report success. */
	if (nr == PR_fsync || nr == PR_fdatasync) {
		struct ukfs_vfd *v = vfd_find(tracee->pid, (int) peek_reg(tracee, CURRENT, SYSARG_1));
		if (!v) return false;
		poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true;
	}
	/* fd-based metadata on a vfd. The placeholder is /dev/null, so fchmod/fchown on
	 * it return EPERM — which breaks tools that set perms via the fd (e.g. git's
	 * adjust_shared_perm on .git/config: "could not write config file ... Operation
	 * not permitted"). Route them to the ukfs path instead. */
	if (nr == PR_fchmod) {
		struct ukfs_vfd *v = vfd_find(tracee->pid, (int) peek_reg(tracee, CURRENT, SYSARG_1));
		if (!v) return false;
		unsigned mode = (unsigned) peek_reg(tracee, CURRENT, SYSARG_2) & 07777;
		char pfx[48]; snprintf(pfx, sizeof pfx, "CHMOD %u ", mode);
		(void) ukfs_simple(pfx, v->path);            /* best-effort; FAT perms are cosmetic */
		poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_fchown) {
		struct ukfs_vfd *v = vfd_find(tracee->pid, (int) peek_reg(tracee, CURRENT, SYSARG_1));
		if (!v) return false;
		unsigned uid = (unsigned) peek_reg(tracee, CURRENT, SYSARG_2);
		unsigned gid = (unsigned) peek_reg(tracee, CURRENT, SYSARG_3);
		char pfx[64]; snprintf(pfx, sizeof pfx, "CHOWN %u %u ", uid, gid);
		(void) ukfs_simple(pfx, v->path);
		poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true;
	}

	/* fstat(fd) on a vfd: report the ukfs file's own attributes, so e.g. the
	 * mount root (the dir fd ls opens) shows the real FS stat, not the host
	 * placeholder fd's stat. (Needs PR_fstat in the seccomp trap set.) */
	if (nr == PR_fstat) {
		struct ukfs_vfd *v = vfd_find(tracee->pid, (int) peek_reg(tracee, CURRENT, SYSARG_1));
		if (!v) return false;
		unsigned mode, uid, gid, nlink; long size, mt, at; unsigned long ino, rdev, blocks;
		if (ukfs_query_stat(v->path, &mode, &uid, &gid, &size, &ino, &mt, &at, &nlink, &rdev, &blocks) != 0)
			return false;
		ukfs_put_stat(tracee, peek_reg(tracee, CURRENT, SYSARG_2),
		              mode, uid, gid, size, ino, mt, at, nlink, rdev, blocks);
		poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true;
	}

	/* stat family on a path under the vmount: answer from ukfsd. Also handles the
	 * empty-path + AT_EMPTY_PATH form (fstatat(fd, "", AT_EMPTY_PATH)), which many
	 * libcs use for fstat — resolve it against the vfd's path. */
	if (nr == PR_newfstatat || nr == PR_fstatat64 || nr == PR_statx) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_2);
		char gp[PATH_MAX], rel[PATH_MAX];
		int have_rel = 0;
		if (pa && read_string(tracee, gp, pa, sizeof gp) > 0 && gp[0] != '\0') {
			have_rel = ukfs_rel_at(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1), gp, rel, sizeof rel);
		} else {
			int flags = (int) peek_reg(tracee, CURRENT, (nr == PR_statx) ? SYSARG_3 : SYSARG_4);
			if (flags & 0x1000 /* AT_EMPTY_PATH */) {
				struct ukfs_vfd *v = vfd_find(tracee->pid, (int) peek_reg(tracee, CURRENT, SYSARG_1));
				if (v) { snprintf(rel, sizeof rel, "%s", v->path); have_rel = 1; }
			}
		}
		if (!have_rel) return false;
		unsigned mode, uid, gid, nlink; long size, mt, at; unsigned long ino, rdev, blocks;
		int q = ukfs_query_stat(rel, &mode, &uid, &gid, &size, &ino, &mt, &at, &nlink, &rdev, &blocks);
		if (q == -1) return false;                 /* socket down: let host try */
		if (q == -2) {
			poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - ENOENT);
			set_sysnum(tracee, PR_void); return true;
		}
		if (nr == PR_statx)
			ukfs_put_statx(tracee, peek_reg(tracee, CURRENT, SYSARG_5),
			               mode, uid, gid, size, ino, mt, at, nlink, rdev, blocks);
		else
			ukfs_put_stat(tracee, peek_reg(tracee, CURRENT, SYSARG_3),
			              mode, uid, gid, size, ino, mt, at, nlink, rdev, blocks);
		poke_reg(tracee, SYSARG_RESULT, 0);
		set_sysnum(tracee, PR_void);
		return true;
	}

	/* xattr ops under the vmount: the FAT/exFAT engine has no xattrs. Without this
	 * the calls fall through to a non-existent host path (ENOENT), which makes ls
	 * print a "?" access-method indicator. getxattr -> ENOTSUP, listxattr -> empty. */
	if (nr == PR_getxattr || nr == PR_lgetxattr) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_1); char gp[PATH_MAX], rel[PATH_MAX];
		if (!pa || read_string(tracee, gp, pa, sizeof gp) <= 0) return false;
		if (!ukfs_rel(gp, rel, sizeof rel)) return false;
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - ENOTSUP); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_listxattr || nr == PR_llistxattr) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_1); char gp[PATH_MAX], rel[PATH_MAX];
		if (!pa || read_string(tracee, gp, pa, sizeof gp) <= 0) return false;
		if (!ukfs_rel(gp, rel, sizeof rel)) return false;
		poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true;   /* no xattrs */
	}

	/* access(2) family under the vmount. access() checks the REAL uid, which under
	 * proot -0 is the (non-root) app uid, so a host-translated check on a file the
	 * FS engine owns as root:0644 would deny write -> e.g. nano reports the file
	 * "unwritable". We are a fake-root view of the FS, so report success for any
	 * existing path (ENOENT only when it truly doesn't exist). access=SYSARG_1 path,
	 * faccessat/faccessat2=SYSARG_2 path. */
	if (nr == PR_access || nr == PR_faccessat || nr == PR_faccessat2) {
		int isaccess = (nr == PR_access);
		word_t pa = peek_reg(tracee, CURRENT, isaccess ? SYSARG_1 : SYSARG_2);
		char gp[PATH_MAX], rel[PATH_MAX];
		if (!pa || read_string(tracee, gp, pa, sizeof gp) <= 0) return false;
		if (!(isaccess ? ukfs_rel(gp, rel, sizeof rel)
		               : ukfs_rel_at(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1), gp, rel, sizeof rel)))
			return false;
		unsigned mode, uid, gid, nlink; long size, mt, at; unsigned long ino, rdev, blocks;
		int q = ukfs_query_stat(rel, &mode, &uid, &gid, &size, &ino, &mt, &at, &nlink, &rdev, &blocks);
		if (q == -1) return false;                 /* socket down: let host try */
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long)(q == 0 ? 0 : -ENOENT));
		set_sysnum(tracee, PR_void); return true;
	}

	/* readlinkat on a symlink under the vmount. */
	if (nr == PR_readlinkat) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_2);
		char gp[PATH_MAX];
		if (!pa || read_string(tracee, gp, pa, sizeof gp) <= 0) return false;
		char rel[PATH_MAX];
		if (!ukfs_rel_at(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1), gp, rel, sizeof rel)) return false;
		char tgt[PATH_MAX];
		long n = ukfs_readlink_at(rel, tgt, sizeof tgt);
		if (n < 0) { poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - EINVAL); set_sysnum(tracee, PR_void); return true; }
		size_t bufsz = (size_t) peek_reg(tracee, CURRENT, SYSARG_4);
		word_t buf = peek_reg(tracee, CURRENT, SYSARG_3);
		if ((size_t) n > bufsz) n = (long) bufsz;
		if (n > 0) write_data(tracee, buf, tgt, (size_t) n);
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) n); set_sysnum(tracee, PR_void); return true;
	}

	/* path-based metadata/namespace ops under the vmount. */
	if (nr == PR_mkdirat) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_2); char gp[PATH_MAX], rel[PATH_MAX];
		if (!pa || read_string(tracee, gp, pa, sizeof gp) <= 0) return false;
		if (!ukfs_rel_at(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1), gp, rel, sizeof rel)) return false;
		unsigned mode = (unsigned) peek_reg(tracee, CURRENT, SYSARG_3) & 07777;
		char pfx[48]; snprintf(pfx, sizeof pfx, "MKDIR %u ", mode);
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) ukfs_simple(pfx, rel)); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_unlinkat) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_2); char gp[PATH_MAX], rel[PATH_MAX];
		if (!pa || read_string(tracee, gp, pa, sizeof gp) <= 0) return false;
		if (!ukfs_rel_at(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1), gp, rel, sizeof rel)) return false;
		int flags = (int) peek_reg(tracee, CURRENT, SYSARG_3);
		int r = ukfs_simple((flags & 0x200) ? "RMDIR " : "UNLINK ", rel);   /* AT_REMOVEDIR */
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) r); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_symlinkat) {
		word_t ta = peek_reg(tracee, CURRENT, SYSARG_1);    /* target, stored verbatim */
		word_t la = peek_reg(tracee, CURRENT, SYSARG_3);    /* link path (an FS path) */
		char target[PATH_MAX], lp[PATH_MAX], rel[PATH_MAX];
		if (!ta || read_string(tracee, target, ta, sizeof target) <= 0) return false;
		if (!la || read_string(tracee, lp, la, sizeof lp) <= 0) return false;
		if (!ukfs_rel_at(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_2), lp, rel, sizeof rel)) return false;
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) ukfs_two_path("SYMLINK", target, rel)); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_fchmodat) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_2); char gp[PATH_MAX], rel[PATH_MAX];
		if (!pa || read_string(tracee, gp, pa, sizeof gp) <= 0) return false;
		if (!ukfs_rel_at(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1), gp, rel, sizeof rel)) return false;
		unsigned mode = (unsigned) peek_reg(tracee, CURRENT, SYSARG_3) & 07777;
		char pfx[48]; snprintf(pfx, sizeof pfx, "CHMOD %u ", mode);
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) ukfs_simple(pfx, rel)); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_fchownat) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_2); char gp[PATH_MAX], rel[PATH_MAX];
		if (!pa || read_string(tracee, gp, pa, sizeof gp) <= 0) return false;
		if (!ukfs_rel_at(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1), gp, rel, sizeof rel)) return false;
		unsigned uid = (unsigned) peek_reg(tracee, CURRENT, SYSARG_3);
		unsigned gid = (unsigned) peek_reg(tracee, CURRENT, SYSARG_4);
		char pfx[64]; snprintf(pfx, sizeof pfx, "CHOWN %u %u ", uid, gid);
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) ukfs_simple(pfx, rel)); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_truncate) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_1); char gp[PATH_MAX], rel[PATH_MAX];
		if (!pa || read_string(tracee, gp, pa, sizeof gp) <= 0) return false;
		if (!ukfs_rel(gp, rel, sizeof rel)) return false;
		long long sz = (long long)(off_t) peek_reg(tracee, CURRENT, SYSARG_2);
		char pfx[48]; snprintf(pfx, sizeof pfx, "TRUNCATE %lld ", sz);
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) ukfs_simple(pfx, rel)); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_renameat || nr == PR_renameat2) {
		word_t oa = peek_reg(tracee, CURRENT, SYSARG_2);
		word_t na = peek_reg(tracee, CURRENT, SYSARG_4);
		char og[PATH_MAX], ng[PATH_MAX], orel[PATH_MAX], nrel[PATH_MAX];
		if (!oa || read_string(tracee, og, oa, sizeof og) <= 0) return false;
		if (!na || read_string(tracee, ng, na, sizeof ng) <= 0) return false;
		int uo = ukfs_rel_at(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1), og, orel, sizeof orel);
		int un = ukfs_rel_at(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_3), ng, nrel, sizeof nrel);
		if (!uo && !un) return false;
		if (uo != un) {     /* across the vmount boundary: let userspace copy+unlink */
			poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - EXDEV); set_sysnum(tracee, PR_void); return true;
		}
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) ukfs_two_path("RENAME", orel, nrel)); set_sysnum(tracee, PR_void); return true;
	}

	/* openat under the vmount: rewrite the path to a real placeholder so the
	 * kernel returns a real fd, and stash the ukfs path for the exit hook.
	 * openat2 carries flags inside a struct open_how pointer (SYSARG_3), so it is
	 * decoded separately below — coreutils/glibc use it (e.g. `cp`), and missing
	 * it sent the dest open straight to the host shadow mountpoint (data loss). */
	if (nr == PR_openat) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_2);
		char gp[PATH_MAX];
		if (!pa || read_string(tracee, gp, pa, sizeof gp) <= 0) return false;
		char rel[PATH_MAX];
		if (!ukfs_rel_at(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1), gp, rel, sizeof rel)) return false;
		int flags = (int) peek_reg(tracee, CURRENT, SYSARG_3);
		int r = ukfs_open_redirect(tracee, rel, flags);
		if (r == 0)
			/* The placeholder is /dev/null (or "/"); strip O_CREAT/O_EXCL/O_TRUNC/
			 * O_DIRECTORY so the real open of it always succeeds. Otherwise an
			 * O_EXCL create (e.g. plain `cp`) hits "File exists" on /dev/null, and
			 * O_DIRECTORY|<file> would wrongly fail. Create/trunc already happened
			 * on the FS side. */
			poke_reg(tracee, SYSARG_3, (word_t)(unsigned)(flags & ~(O_CREAT | O_EXCL | O_TRUNC | O_DIRECTORY)));
		return r == 1;
	}
	if (nr == PR_openat2) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_2);
		char gp[PATH_MAX];
		if (!pa || read_string(tracee, gp, pa, sizeof gp) <= 0) return false;
		char rel[PATH_MAX];
		if (!ukfs_rel_at(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1), gp, rel, sizeof rel)) return false;
		/* struct open_how { __u64 flags; __u64 mode; __u64 resolve; } at SYSARG_3,
		 * size in SYSARG_4. We only need flags (and let create use the default mode). */
		word_t howp = peek_reg(tracee, CURRENT, SYSARG_3);
		word_t howsz = peek_reg(tracee, CURRENT, SYSARG_4);
		unsigned long long how[3] = { 0, 0, 0 };
		size_t n = (size_t) howsz; if (n > sizeof how) n = sizeof how;
		if (!howp || n < 8 || read_data(tracee, how, howp, n) < 0) return false;
		int r = ukfs_open_redirect(tracee, rel, (int) how[0]);
		if (r == 0) {
			/* Same placeholder-flag strip as openat, plus clear the resolve field
			 * so the absolute /dev/null placeholder isn't rejected by a
			 * RESOLVE_BENEATH/RESOLVE_IN_ROOT restriction. Write open_how back. */
			how[0] &= ~(unsigned long long)(O_CREAT | O_EXCL | O_TRUNC | O_DIRECTORY);
			how[2] = 0;
			write_data(tracee, howp, how, n);
		}
		return r == 1;
	}

	return false;
}

/* Called from translate_syscall_exit (exit.c) for every syscall exit: binds the
 * fd returned by a redirected openat to its ukfs path in the vfd table. */
void uknl_fs_open_exit(Tracee *tracee, word_t nr)
{
	if (!uk_fs_on()) return;
	int pid = tracee->pid;

	/* dup/dup2/dup3 of a vfd: make the new fd number alias the same ukfs path.
	 * Without this, a write through a redirected fd is lost: `echo > file` opens
	 * the file (vfd), dup2()s it onto stdout (fd 1), closes the original, then
	 * writes to fd 1 — which, untracked, hits the /dev/null placeholder and the
	 * data vanishes (the file stays empty). On aarch64 dup2 is implemented via
	 * dup3, so both are handled. */
	if (nr == PR_dup || nr == PR_dup2 || nr == PR_dup3) {
		long newfd = (long)(int) peek_reg(tracee, CURRENT, SYSARG_RESULT);
		if (newfd < 0) return;                       /* dup failed: nothing changed */
		/* At sysEXIT the arg registers are clobbered (on aarch64 x0 holds the
		 * return value), so read oldfd from the ORIGINAL (entry) snapshot. */
		int oldfd = (int) peek_reg(tracee, ORIGINAL, SYSARG_1);
		if ((int) newfd == oldfd) return;            /* dup2(fd,fd): no-op */
		/* dup2/dup3 REDEFINE newfd, so any vfd previously on that fd number is now
		 * stale and MUST be dropped — e.g. the shell restoring stdout with
		 * dup2(saved_tty_fd, 1) after `echo > file`: without this, vfd[1] keeps
		 * pointing at the file and every later write to fd 1 (the prompt, all
		 * command output) is redirected into the file (terminal "freezes"). */
		struct ukfs_vfd *old = vfd_find(pid, (int) newfd);
		if (old) vfd_free(old);
		/* If the source is one of our vfds, the new fd aliases the same path. */
		struct ukfs_vfd *src = vfd_find(pid, oldfd);
		if (!src) return;
		struct ukfs_vfd *v = vfd_alloc();
		if (v) {
			memset(v, 0, sizeof *v);
			v->used = 1; v->pid = pid; v->fd = (int) newfd;
			v->isdir = src->isdir; v->off = src->off;
			snprintf(v->path, sizeof v->path, "%s", src->path);
		}
		return;
	}

	if (nr != PR_openat && nr != PR_openat2) return;
	struct ukfs_pending *pp = NULL;
	for (int i = 0; i < 64; i++)
		if (g_open_pending[i].used && g_open_pending[i].pid == pid) { pp = &g_open_pending[i]; break; }
	if (!pp) return;
	long fd = (long)(int) peek_reg(tracee, CURRENT, SYSARG_RESULT);
	if (fd >= 0) {
		struct ukfs_vfd *v = vfd_find(pid, (int) fd);   /* recycle a stale fd number */
		if (v) vfd_free(v);
		v = vfd_alloc();
		if (v) {
			memset(v, 0, sizeof *v);
			v->used = 1; v->pid = pid; v->fd = (int) fd; v->isdir = pp->isdir; v->off = 0;
			snprintf(v->path, sizeof v->path, "%s", pp->path);
		}
	}
	pp->used = 0;
}
