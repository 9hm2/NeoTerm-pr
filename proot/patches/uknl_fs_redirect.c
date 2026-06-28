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
static void uk_dbg(Tracee *tracee, const char *line);   /* fwd (defined below) */
static void uk_dbgv_line(const char *line);             /* verbose: only if UK_FS_DEBUG */
static void uk_dbgv(Tracee *tracee, const char *line);  /* verbose: only if UK_FS_DEBUG */
static int  ukfs_tgid(int tid);                          /* fwd (defined below) */
static void ukfs_canon(const char *in, char *out, size_t osz);  /* fwd (defined below) */
static void ukfs_unmount_slot(int i);                           /* fwd (defined below) */
static void fch_forget_forks(int c);                            /* fwd (defined below) */

/* ---- io.neoterm.fs connections — one MOUNT per connection, one connection per
 * mounted partition. A multi-partition device (e.g. a Raspberry Pi card: FAT boot
 * + ext4 root) is exposed as /dev/uksd0pN; each guest mount gets its own slot
 * here, its own ukfsd (listening on io.neoterm.fs.pN), and its own connection.
 * The whole-device /dev/uksd0 maps to io.neoterm.fs. g_am ("active mount") is set
 * by ukfs_rel() / the fd-op path BEFORE each ukfsd request, so ukfs_conn() talks
 * to the right daemon. proot's tracer is single-threaded (one tracee syscall at a
 * time), so a global active-mount index is race-free. */
#define UK_MAXM 8
struct ukm {
	int  used;
	char vmount[PATH_MAX];   /* guest mount point, e.g. "/mnt/boot" */
	char token[600];         /* MOUNT token: "uksd0"/"uksd0pN" (USB), or an absolute
	                          * HOST image path (loop). dev_path passes the path through. */
	char fsname[64];         /* ukfsd daemon socket allocated from the pool */
	long long base, size;    /* explicit byte range (loop / partition of a file) */
	int  range;              /* send "MOUNT auto <token> <base> <size>" when set */
	int  sock;               /* this mount's connection (-1 = none) */
	int  ready;              /* ukfsd has MOUNTed on sock */
	/* FUSE-backed mount (libfuse daemon over our /dev/fuse channel). When fuse != 0
	 * this slot routes path ops through the fused engine instead of ukfsd. */
	fused_t *fuse;           /* the FUSE "kernel" engine, or NULL for block/ukfsd */
	int  fuse_chan;          /* the channel fd (our end of the daemon's /dev/fuse) */
	int  fuse_inited;        /* FUSE_INIT done (lazy, on first access after mount) */
};
static struct ukm g_m[UK_MAXM];
static int g_nm = 0;             /* number of registered mounts */
static int g_am = -1;            /* active mount index (set before each request) */

/* The active mount's FUSE engine (or NULL if it's a block/ukfsd mount). On the
 * first access after mount, run the deferred FUSE_INIT handshake — it can't be
 * done in the mount hook (the daemon is still inside mount(2) then and not yet
 * reading its channel, so INIT's reply would never come). g_am is set by
 * ukfs_rel()/vfd_lookup before each op, same as the ukfsd path. */
static fused_t *fuse_active(void)
{
	if (g_am < 0 || g_am >= UK_MAXM || !g_m[g_am].used || !g_m[g_am].fuse) return NULL;
	struct ukm *m = &g_m[g_am];
	if (!m->fuse_inited) {
		if (fused_init(m->fuse) != 0) return NULL;
		m->fuse_inited = 1;
	}
	return m->fuse;
}

/* ===== /dev/fuse channel (phase 2, marker + in-proot bridge) =====
 * A guest libfuse daemon does open("/dev/fuse") then mount(2). There is no kernel
 * FUSE it can use, so we make /dev/fuse OURS — WITHOUT morphing the open syscall
 * (which is unreliable: changing the syscall number at the ptrace stop hits
 * proot's sysnum-change workaround). Instead open("/dev/fuse") opens the bound
 * marker normally and returns a real fd; we then intercept the daemon's
 * read()/write() on that fd EXACTLY like the /dev/uksd0 block proxy (read/write
 * are reliably trapped, FILTER_SYSEXIT) and bridge them to the `fused` engine
 * through an in-proot channel. The accessor side (a path op under the FUSE mount)
 * and the daemon side (read/write on /dev/fuse) PUMP the event loop for each
 * other, so the single-threaded tracer never deadlocks. */
#include <stddef.h>
#include <time.h>

#define UK_NFCH 8
#define UK_FUSE_MSGCAP (132 * 1024)   /* >= fused's FUSED_MSGCAP (one FUSE message) */
/* One FUSE channel, MULTIPLEXED: several processes can use one FUSE mount at once
 * (e.g. rpi-imager + lsblk both read the squashfs), and the redirect's event-loop
 * pump interleaves their ops on the SAME engine. So we queue outstanding requests
 * (rq, FIFO) and match the daemon's replies to the waiting op by `unique` (rp),
 * exactly as the kernel FUSE driver multiplexes — no crossed/clobbered messages. */
#define UK_FQ 32
struct ukfmsg { int used; uint64_t uniq; uint64_t seq; unsigned char *buf; int len; };
struct ukfch {
	int used, pid, fd;
	fused_t *eng;
	struct ukfmsg rq[UK_FQ];   /* pending requests (oldest seq served first) */
	struct ukfmsg rp[UK_FQ];   /* replies, claimed by matching unique */
	uint64_t seq;
	/* The daemon's read(/dev/fuse) is "parked" (left stopped) when the request
	 * queue is empty, so the single-threaded tracer doesn't deadlock; it is resumed
	 * when an accessor enqueues a request. dvec = iovcnt for a parked readv(), -1
	 * for a plain read(). */
	Tracee *dmn; word_t dbuf; int dcnt; int dvec; int parked;
	int owner;          /* pid that open()ed /dev/fuse (squashfuse helper) */
	int owner2;         /* its parent (the AppImage runtime that exec()s the app) */
	int app_pid;        /* owner/owner2 that exec()'d the app; teardown when IT exits */
	char marker[256];   /* host path the /dev/fuse fd resolves to (survives fork) */
};
static struct ukfch g_fch[UK_NFCH];
static int g_fopen_tid[UK_NFCH];          /* this thread's open() is for /dev/fuse */
static int ukfs_tgid(int tid);            /* fwd (defined below) */

/* Fork hints: child-pid -> channel index. A FUSE daemon usually daemonizes
 * (fork; parent exits; the CHILD runs the session loop on the inherited /dev/fuse
 * fd). Several mounts bind the SAME /dev/fuse marker, so once the parent is gone
 * the marker path can't tell the channels apart. We instead record the fork at
 * fork time (parent still alive, see uknl_fuse_note_fork) so the child resolves to
 * the right channel. */
#define UK_NFORK 64
static struct { int used, pid, chan; } g_fch_fork[UK_NFORK];

/* One-step event-loop pump (injected into tracee/event.c): handle one other
 * tracee's stop. Used to let an accessor wait while the FUSE daemon runs. Returns
 * 0, or -1 if there are no more children. */
extern int uknl_pump_one(void);
/* restart_tracee() + push_specific_regs() are proot core (declared by the headers
 * enter.c already includes); used to resume a parked daemon read. */

/* A daemon read(/dev/fuse) we left stopped (parked): the event loop must skip its
 * restart so it stays put until fch_send wakes it. The redirect sets this; the
 * patched restart sites (event_loop + pump_one) consult uknl_fuse_take_park(). */
static int g_fuse_park_pid = 0;
/* Latched once the root (session-leader) tracee exits: from then on a daemon read
 * that finds no request must NOT park (nobody will ever wake it) — it gets EOF so
 * the server exits. Closes the race where a daemon re-parks just after the drain. */
static int g_fuse_root_gone = 0;
int uknl_fuse_take_park(int pid);
int uknl_fuse_take_park(int pid)
{
	if (g_fuse_park_pid && g_fuse_park_pid == pid) { g_fuse_park_pid = 0; return 1; }
	return 0;
}

/* Scatter `n` bytes of `src` into a (possibly vectored) guest buffer of tracee t.
 * vec<0: plain buffer at `a2`; vec>=0: `vec` iovecs at `a2`. */
static void fch_scatter(Tracee *t, word_t a2, int vec, const unsigned char *src, size_t n)
{
	if (vec < 0) { if (n) write_data(t, a2, src, n); return; }
	size_t off = 0;
	for (int i = 0; i < vec && off < n; i++) {
		word_t iov[2];
		if (read_data(t, iov, a2 + (word_t) i * 2 * sizeof(word_t), 2 * sizeof(word_t)) < 0) break;
		size_t chunk = (size_t) iov[1]; if (chunk > n - off) chunk = n - off;
		if (chunk) write_data(t, iov[0], src + off, chunk);
		off += chunk;
	}
}

/* fuse_in_header / fuse_out_header both carry `unique` at byte offset 8. */
static uint64_t fch_uniq(const unsigned char *b, int len)
{
	uint64_t u = 0; if (len >= 16) memcpy(&u, b + 8, 8); return u;
}

/* Index of the oldest queued request (lowest seq), or -1 if none. */
static int fch_oldest_rq(struct ukfch *ch)
{
	int best = -1; uint64_t bs = ~0ULL;
	for (int i = 0; i < UK_FQ; i++)
		if (ch->rq[i].used && ch->rq[i].seq < bs) { bs = ch->rq[i].seq; best = i; }
	return best;
}

/* Deliver the oldest queued request to a parked daemon read() and resume it. */
static void fch_deliver_parked(struct ukfch *ch)
{
	if (!ch->parked || !ch->dmn) return;
	int i = fch_oldest_rq(ch);
	if (i < 0) return;
	struct ukfmsg *m = &ch->rq[i];
	size_t n = (size_t) m->len; if (n > (size_t) ch->dcnt) n = (size_t) ch->dcnt;
	fch_scatter(ch->dmn, ch->dbuf, ch->dvec, m->buf, n);
	poke_reg(ch->dmn, SYSARG_RESULT, (word_t) n);
	set_sysnum(ch->dmn, PR_void);
	/* The read's exit-restore copies the result from the MODIFIED snapshot. */
	save_current_regs(ch->dmn, MODIFIED);
	free(m->buf); m->buf = NULL; m->used = 0;
	ch->parked = 0;
	Tracee *d = ch->dmn; ch->dmn = NULL;
	(void) push_specific_regs(d, 1);
	(void) restart_tracee(d, 0);
}

/* Resume a parked daemon read() with EOF (0 bytes). libfuse's session loop treats
 * a zero-length read from /dev/fuse as "channel closed" and exits, so the daemon
 * process can terminate. Without this a daemonized FUSE server left parked
 * (ptrace-stopped, never resumed) lingers forever and blocks proot from exiting —
 * the guest terminal won't restart. Called on unmount. */
static void fch_resume_eof(struct ukfch *ch)
{
	if (!ch->parked || !ch->dmn) return;
	poke_reg(ch->dmn, SYSARG_RESULT, 0);
	set_sysnum(ch->dmn, PR_void);
	save_current_regs(ch->dmn, MODIFIED);
	ch->parked = 0;
	Tracee *d = ch->dmn; ch->dmn = NULL;
	(void) push_specific_regs(d, 1);
	(void) restart_tracee(d, 0);
}

/* Resume EVERY parked FUSE daemon with EOF. Called when the root (session-leader)
 * tracee exits: a daemonized FUSE server (rpi-imager's squashfuse, sshfs -o
 * daemon, …) that is still parked in read(/dev/fuse) would otherwise stay
 * ptrace-stopped forever, so proot's event loop never sees ECHILD and never
 * exits — the terminal hangs and won't restart. EOF makes libfuse leave its
 * session loop and the server process exit, so proot can finish. */
void uknl_fuse_drain_parked(void);
void uknl_fuse_drain_parked(void)
{
	g_fuse_root_gone = 1;          /* any later park -> EOF (see read path) */
	for (int i = 0; i < UK_NFCH; i++)
		if (g_fch[i].used && g_fch[i].parked && g_fch[i].dmn)
			fch_resume_eof(&g_fch[i]);
}

/* `pid` exec()'d an app. If pid is a channel's opener (squashfuse helper) or that
 * opener's parent (the AppImage runtime), record pid as the channel's app: the
 * AppImage flow is open(/dev/fuse) -> mount -> fork daemon -> exec AppRun, where
 * the execer becomes the foreground app. Recording the EXECER (not just any owner)
 * matters: the opener also exits when it merely daemonizes (sshfs), which must NOT
 * trigger teardown — only the app's exit does (see uknl_fuse_owner_gone). */
void uknl_fuse_note_exec(int pid);
void uknl_fuse_note_exec(int pid)
{
	for (int i = 0; i < UK_NFCH; i++)
		if (g_fch[i].used && g_fch[i].app_pid == 0 &&
		    (g_fch[i].owner == pid || g_fch[i].owner2 == pid)) {
			g_fch[i].app_pid = pid;
			char l[80]; snprintf(l, sizeof l, "uk_fs: fuse app=%d chan=%d\n", pid, i); uk_dbg_line(l);
		}
}

/* The app that owns a FUSE mount exited (e.g. rpi-imager ^C'd). Tear the mount
 * down so its squashfuse daemon stops (freeing the squashfs it had mmap'd) instead
 * of lingering parked and mounted — repeated interrupt+relaunch otherwise piles up
 * mounts until memory pressure makes new launches fail ("file too short"). Only the
 * exec()ing app pid triggers teardown, so a daemonizing FUSE (sshfs), whose opener
 * exits by design without exec()ing an app, is left alone. */
void uknl_fuse_owner_gone(int pid);
void uknl_fuse_owner_gone(int pid)
{
	for (int i = 0; i < UK_NFCH; i++) {
		if (!g_fch[i].used || g_fch[i].app_pid != pid) continue;
		{ char l[80]; snprintf(l, sizeof l, "uk_fs: fuse owner gone pid=%d chan=%d -> teardown\n", pid, i); uk_dbg_line(l); }
		int found = 0;
		for (int s = 0; s < UK_MAXM; s++)
			if (g_m[s].used && g_m[s].fuse && g_m[s].fuse_chan == i) { ukfs_unmount_slot(s); found = 1; break; }
		if (!found && g_fch[i].used) {           /* mount(2) hadn't run yet: free the channel */
			fch_resume_eof(&g_fch[i]);
			if (g_fch[i].eng) fused_destroy(g_fch[i].eng);
			for (int k = 0; k < UK_FQ; k++) { free(g_fch[i].rq[k].buf); free(g_fch[i].rp[k].buf); }
			fch_forget_forks(i);
			memset(&g_fch[i], 0, sizeof g_fch[i]);
		}
	}
}

/* True if `pid` has an unblocked, pending fatal default signal (INT/TERM/HUP/
 * QUIT/KILL). A parked daemon is ptrace-stopped and can't act on a signal while
 * we hold it, so e.g. ^C on the app (SIGINT to the group) just queues on the
 * squashfuse daemon and the mount leaks. We poll this to release it. */
static int fch_has_fatal_pending(int pid)
{
	char p[64]; snprintf(p, sizeof p, "/proc/%d/status", pid);
	int fd = open(p, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return 0;
	char b[4096]; ssize_t n = read(fd, b, sizeof b - 1); close(fd);
	if (n <= 0) return 0;
	b[n] = '\0';
	unsigned long long pnd = 0, shd = 0, blk = 0; char *s;
	if ((s = strstr(b, "SigPnd:"))) pnd = strtoull(s + 7, NULL, 16);
	if ((s = strstr(b, "ShdPnd:"))) shd = strtoull(s + 7, NULL, 16);
	if ((s = strstr(b, "SigBlk:"))) blk = strtoull(s + 7, NULL, 16);
	unsigned long long fatal =
		(1ULL << (SIGINT - 1)) | (1ULL << (SIGTERM - 1)) |
		(1ULL << (SIGHUP - 1)) | (1ULL << (SIGQUIT - 1)) | (1ULL << (SIGKILL - 1));
	return (((pnd | shd) & fatal) & ~blk) != 0;     /* KILL is never in SigBlk */
}

/* Release any parked daemon that has a pending fatal signal: hand it EOF so
 * libfuse leaves its session loop, unmounts (-> our umount hook frees the
 * channel + mount) and the process exits. Without this an interrupted app
 * (rpi-imager ^C'd) leaves its squashfuse parked and mounted forever, and the
 * mounts pile up until new launches misbehave. Cheap no-op when nothing parked. */
void uknl_fuse_reap_signaled(void);
void uknl_fuse_reap_signaled(void)
{
	for (int i = 0; i < UK_NFCH; i++)
		if (g_fch[i].used && g_fch[i].parked && g_fch[i].dmn &&
		    fch_has_fatal_pending(g_fch[i].dmn->pid))
			fch_resume_eof(&g_fch[i]);
}

static struct ukfch *fch_by_fd(int pid, int fd)
{
	for (int i = 0; i < UK_NFCH; i++)
		if (g_fch[i].used && g_fch[i].pid == pid && g_fch[i].fd == fd) return &g_fch[i];
	return NULL;
}

/* Channel index owned by tgid `pid` — directly, or via a recorded fork hint. */
static int fch_chan_of_pid(int pid)
{
	for (int i = 0; i < UK_NFCH; i++)
		if (g_fch[i].used && g_fch[i].eng && g_fch[i].pid == pid) return i;
	for (int i = 0; i < UK_NFORK; i++)
		if (g_fch_fork[i].used && g_fch_fork[i].pid == pid) return g_fch_fork[i].chan;
	return -1;
}

/* Drop every fork hint for channel index `c` (called when the channel is torn
 * down or its slot reused, so a stale hint can't mis-adopt a later process). */
static void fch_forget_forks(int c)
{
	for (int i = 0; i < UK_NFORK; i++)
		if (g_fch_fork[i].used && g_fch_fork[i].chan == c) g_fch_fork[i].used = 0;
}

/* A traced process forked. If the parent owns (or inherited) a FUSE channel, the
 * child inherits its /dev/fuse fd; record child->channel so the child resolves to
 * the RIGHT channel later — marker-path matching can't disambiguate several mounts
 * that share the /dev/fuse marker. Injected into proot's new_child(). */
void uknl_fuse_note_fork(int ppid, int cpid);
void uknl_fuse_note_fork(int ppid, int cpid)
{
	int c = fch_chan_of_pid(ppid);
	if (c < 0) c = fch_chan_of_pid(ukfs_tgid(ppid));
	if (c < 0) return;
	int slot = -1;
	for (int i = 0; i < UK_NFORK; i++) {
		if (g_fch_fork[i].used && g_fch_fork[i].pid == cpid) { slot = i; break; }
		if (slot < 0 && !g_fch_fork[i].used) slot = i;
	}
	if (slot < 0) slot = 0;                 /* table full: clobber the first slot */
	g_fch_fork[slot].used = 1; g_fch_fork[slot].pid = cpid; g_fch_fork[slot].chan = c;
}

/* True iff `pid` belongs to a FUSE daemon's thread group (it owns a channel).
 * The re-entrant pump (fch_recv) must advance ONLY the daemon while an accessor
 * waits for a reply; every OTHER tracee's stop is deferred to the real event loop,
 * else consuming e.g. a sibling's exit here breaks proot's parent-wait emulation
 * and the guest shell's wait(2) hangs. Fast path on raw pid, then thread group. */
int uknl_fuse_is_daemon(int pid);
int uknl_fuse_is_daemon(int pid)
{
	for (int i = 0; i < UK_NFCH; i++)
		if (g_fch[i].used && g_fch[i].eng && g_fch[i].pid == pid) return 1;
	int tg = ukfs_tgid(pid);
	if (tg != pid)
		for (int i = 0; i < UK_NFCH; i++)
			if (g_fch[i].used && g_fch[i].eng && g_fch[i].pid == tg) return 1;
	return 0;
}

/* Parent pid (ppid) of `pid` from /proc/<pid>/stat (0 on failure). The comm
 * field may contain spaces/parens, so parse after the last ')'. */
static int fch_ppid(int pid)
{
	char p[64]; snprintf(p, sizeof p, "/proc/%d/stat", pid);
	int fd = open(p, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return 0;
	char b[512]; ssize_t n = read(fd, b, sizeof b - 1); close(fd);
	if (n <= 0) return 0;
	b[n] = '\0';
	char *r = strrchr(b, ')');
	if (!r) return 0;
	char state; int ppid = 0;
	if (sscanf(r + 1, " %c %d", &state, &ppid) == 2) return ppid;
	return 0;
}

/* readlink /proc/<tid>/fd/<fd> -> host path (or "" on failure). */
static void fch_fdpath(int tid, int fd, char *out, size_t cap)
{
	char lk[64]; snprintf(lk, sizeof lk, "/proc/%d/fd/%d", tid, fd);
	ssize_t n = readlink(lk, out, cap - 1);
	out[(n > 0) ? n : 0] = '\0';
}

/* Find the channel for a daemon's /dev/fuse fd, tolerating fork/daemonization:
 * a forked child inherits the fd but has a new tgid. Match by (tgid,fd) first;
 * else by a fork hint recorded when the daemon forked (the reliable way to tell
 * apart several mounts that share the /dev/fuse marker). A daemon whose channel
 * is gone resolves to nothing and reads the real (empty) marker -> natural EOF. */
static struct ukfch *fch_resolve(Tracee *tracee, int fd)
{
	int tg = ukfs_tgid(tracee->pid);
	struct ukfch *ch = fch_by_fd(tg, fd);
	if (ch) return ch;
	/* Fast path: only pay for the readlink()-based fork adoption if SOME channel
	 * actually uses this fd number (rare). Otherwise this is an ordinary read/write
	 * on a non-FUSE fd — return immediately, no syscall. Without this, every
	 * read/write system-wide would do a readlink while a FUSE mount is active,
	 * which times out heavy /sys scanners like lsblk. */
	int maybe = 0;
	for (int i = 0; i < UK_NFCH; i++)
		if (g_fch[i].used && g_fch[i].eng && g_fch[i].fd == fd) { maybe = 1; break; }
	if (!maybe) return NULL;
	/* Fork hint first: the reliable disambiguator when several mounts share the
	 * /dev/fuse marker. Recorded at fork time, so it survives the parent's exit. */
	for (int i = 0; i < UK_NFORK; i++)
		if (g_fch_fork[i].used && (g_fch_fork[i].pid == tg || g_fch_fork[i].pid == tracee->pid)) {
			int c = g_fch_fork[i].chan;
			if (c >= 0 && g_fch[c].used && g_fch[c].eng && g_fch[c].fd == fd) {
				char l[80]; snprintf(l, sizeof l, "uk_fs: fuse adopt fd=%d pid=%d (fork hint)\n", fd, tg); uk_dbg_line(l);
				g_fch[c].pid = tg; return &g_fch[c];
			}
		}
	/* No channel: the fork hint is authoritative (every fork is recorded at fork
	 * time by uknl_fuse_note_fork). We deliberately do NOT fall back to marker-path
	 * matching: with several mounts sharing the /dev/fuse marker it's ambiguous, and
	 * a just-unmounted mount's dying daemon could otherwise re-adopt a surviving
	 * channel and corrupt it. A daemon whose channel is gone returns NULL here, so
	 * its read runs on the real (empty) marker file and gets a natural EOF — exactly
	 * what makes it leave its session loop and exit. */
	return NULL;
}

/* fused transport: enqueue one request; if the daemon is parked in a blocking
 * read(), hand it the oldest queued request and resume it. */
static int fch_send(void *c, const void *buf, size_t len)
{
	struct ukfch *ch = c;
	if (len > UK_FUSE_MSGCAP) return -EIO;
	int i; for (i = 0; i < UK_FQ; i++) if (!ch->rq[i].used) break;
	if (i == UK_FQ) return -EIO;                       /* request queue full */
	unsigned char *b = malloc(len);
	if (!b) return -ENOMEM;
	memcpy(b, buf, len);
	ch->rq[i].used = 1; ch->rq[i].buf = b; ch->rq[i].len = (int) len;
	ch->rq[i].uniq = fch_uniq(buf, (int) len); ch->rq[i].seq = ++ch->seq;
	if (ch->parked) fch_deliver_parked(ch);
	return (int) len;
}
/* fused transport: wait for the daemon's reply that matches `unique`, pumping the
 * loop so the daemon (and other ops) run. g_am is global and the pump may serve
 * another op, so save/restore it. */
static int fch_recv(void *c, void *buf, size_t cap, uint64_t unique)
{
	struct ukfch *ch = c;
	int saved = g_am;
	for (;;) {
		for (int i = 0; i < UK_FQ; i++)
			if (ch->rp[i].used && ch->rp[i].uniq == unique) {
				int n = ch->rp[i].len; if ((size_t) n > cap) n = (int) cap;
				memcpy(buf, ch->rp[i].buf, n);
				free(ch->rp[i].buf); ch->rp[i].buf = NULL; ch->rp[i].used = 0;
				g_am = saved; return n;
			}
		if (uknl_pump_one() < 0) { g_am = saved; return -EIO; }
	}
}

/* enter: remember this thread is opening /dev/fuse (NO morph; the marker open
 * proceeds and its real fd is captured at exit). */
static void uknl_fuse_open_enter(Tracee *tracee)
{
	for (int i = 0; i < UK_NFCH; i++) if (!g_fopen_tid[i]) { g_fopen_tid[i] = tracee->pid; return; }
}
/* exit: bind the returned fd as a FUSE channel + build the fused engine on it. */
static void uknl_fuse_open_exit(Tracee *tracee)
{
	int slot = -1;
	for (int i = 0; i < UK_NFCH; i++) if (g_fopen_tid[i] == tracee->pid) { slot = i; break; }
	if (slot < 0) return;
	g_fopen_tid[slot] = 0;
	long fd = (long)(int) peek_reg(tracee, CURRENT, SYSARG_RESULT);
	if (fd < 0) return;
	int pid = ukfs_tgid(tracee->pid);
	struct ukfch *ch = fch_by_fd(pid, (int) fd);          /* recycle a stale fd number */
	if (!ch) for (int i = 0; i < UK_NFCH; i++) if (!g_fch[i].used) { ch = &g_fch[i]; break; }
	if (!ch) return;
	if (ch->eng) fused_destroy(ch->eng);
	for (int i = 0; i < UK_FQ; i++) { free(ch->rq[i].buf); free(ch->rp[i].buf); }
	fch_forget_forks((int)(ch - g_fch));               /* drop any stale hints for this slot */
	memset(ch, 0, sizeof *ch);
	ch->eng = fused_new(-1, 0, 0);                     /* fd unused: io is the channel */
	if (!ch->eng) { memset(ch, 0, sizeof *ch); return; }
	fused_set_io(ch->eng, fch_send, fch_recv, ch);
	ch->used = 1; ch->pid = pid; ch->fd = (int) fd;
	/* The app that owns this mount (see owner_gone): the opener (squashfuse helper)
	 * AND its parent (the AppImage runtime that exec()s the real app), so teardown
	 * works whether the runtime mounts directly or via a forked helper. */
	ch->owner = pid; ch->owner2 = fch_ppid(tracee->pid); ch->app_pid = 0;
	fch_fdpath(tracee->pid, (int) fd, ch->marker, sizeof ch->marker);   /* for fork adoption */
	{ char l[96]; snprintf(l, sizeof l, "uk_fs: /dev/fuse open fd=%ld pid=%d ppid=%d\n", fd, pid, ch->owner2); uk_dbg(tracee, l); }
}

/* The daemon's read()/write() on a /dev/fuse channel fd (intercepted like the
 * block proxy). read delivers the queued request (pumping until one exists);
 * write deposits the reply. Returns true if handled (PR_void'd + result poked). */
static bool uknl_fuse_chan_io(Tracee *tracee, word_t nr)
{
	int is_read  = (nr == PR_read || nr == PR_pread64 || nr == PR_readv);
	int is_write = (nr == PR_write || nr == PR_pwrite64 || nr == PR_writev);
	int is_vec   = (nr == PR_readv || nr == PR_writev);
	if (!is_read && !is_write) return false;
	int fd = (int) peek_reg(tracee, CURRENT, SYSARG_1);
	struct ukfch *ch = fch_resolve(tracee, fd);   /* tolerates fork/daemonize */
	if (!ch) return false;
	word_t a2 = peek_reg(tracee, CURRENT, SYSARG_2);   /* buf, or iov base */
	size_t a3 = (size_t) peek_reg(tracee, CURRENT, SYSARG_3);   /* count, or iovcnt */

	/* Total capacity across a (possibly vectored) buffer, and a scatter/gather
	 * helper. iovec is {ptr,len} = 2 words. */
	if (is_read) {
		/* compute total readable capacity */
		size_t cap = 0;
		if (is_vec) {
			for (size_t i = 0; i < a3; i++) {
				word_t iov[2];
				if (read_data(tracee, iov, a2 + i * 2 * sizeof(word_t), 2 * sizeof(word_t)) < 0) break;
				cap += (size_t) iov[1];
			}
		} else cap = a3;
		int qi = fch_oldest_rq(ch);
		if (qi >= 0) {
			struct ukfmsg *m = &ch->rq[qi];
			size_t n = (size_t) m->len; if (n > cap) n = cap;
			fch_scatter(tracee, a2, is_vec ? (int) a3 : -1, m->buf, n);
			free(m->buf); m->buf = NULL; m->used = 0;
			poke_reg(tracee, SYSARG_RESULT, (word_t) n); set_sysnum(tracee, PR_void); return true;
		}
		/* Root already exited: nobody will ever send a request, so do NOT park
		 * (it would never be woken). Hand the daemon EOF so libfuse exits. */
		if (g_fuse_root_gone) {
			poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true;
		}
		/* queue empty: PARK this read until a request is enqueued. */
		ch->dmn = tracee; ch->dbuf = a2; ch->dcnt = (int) cap; ch->parked = 1; ch->dvec = is_vec ? (int) a3 : -1;
		g_fuse_park_pid = tracee->pid;
		return true;
	}
	/* write: the daemon's reply — gather, then queue keyed by unique */
	unsigned char *rb = malloc(UK_FUSE_MSGCAP);
	if (!rb) { poke_reg(tracee, SYSARG_RESULT, (word_t)(long) -ENOMEM); set_sysnum(tracee, PR_void); return true; }
	size_t total = 0;
	if (is_vec) {
		for (size_t i = 0; i < a3; i++) {
			word_t iov[2];
			if (read_data(tracee, iov, a2 + i * 2 * sizeof(word_t), 2 * sizeof(word_t)) < 0) break;
			size_t chunk = (size_t) iov[1]; if (chunk > UK_FUSE_MSGCAP - total) chunk = UK_FUSE_MSGCAP - total;
			if (chunk) read_data(tracee, rb + total, iov[0], chunk);
			total += chunk;
		}
	} else {
		total = a3; if (total > UK_FUSE_MSGCAP) total = UK_FUSE_MSGCAP;
		if (read_data(tracee, rb, a2, total) < 0) { free(rb); poke_reg(tracee, SYSARG_RESULT, (word_t)(long) -EFAULT); set_sysnum(tracee, PR_void); return true; }
	}
	int ri; for (ri = 0; ri < UK_FQ; ri++) if (!ch->rp[ri].used) break;
	if (ri == UK_FQ) free(rb);   /* reply queue full: drop (op will -EIO/retry) */
	else { ch->rp[ri].used = 1; ch->rp[ri].buf = rb; ch->rp[ri].len = (int) total; ch->rp[ri].uniq = fch_uniq(rb, (int) total);}
	/* return the count the daemon asked to write (its full reply) */
	{ size_t reported = is_vec ? total : a3; poke_reg(tracee, SYSARG_RESULT, (word_t) reported); }
	set_sysnum(tracee, PR_void); return true;
}

/* Register a FUSE-backed vmount (target -> the channel's fused engine). chanidx
 * links the mount to its channel so unmount can free both. */
static int ukm_register_fuse(const char *tgt, fused_t *eng, int chanidx)
{
	int slot = -1;
	for (int i = 0; i < UK_MAXM; i++) if (g_m[i].used && strcmp(g_m[i].vmount, tgt) == 0) { slot = i; break; }
	if (slot < 0) for (int i = 0; i < UK_MAXM; i++) if (!g_m[i].used) { slot = i; break; }
	if (slot < 0) return -1;
	if (!g_m[slot].used) g_nm++;
	memset(&g_m[slot], 0, sizeof g_m[slot]);
	g_m[slot].used = 1; g_m[slot].sock = -1;
	g_m[slot].fuse = eng; g_m[slot].fuse_chan = chanidx; g_m[slot].fuse_inited = 0;
	snprintf(g_m[slot].vmount, sizeof g_m[slot].vmount, "%s", tgt);
	return slot;
}

/* mount(2) of a FUSE filesystem: fstype "fuse"/"fuse.*"/"fuseblk", options carry
 * "fd=N" (the daemon's /dev/fuse fd). Bind that channel's engine as the vmount.
 * FUSE_INIT is deferred to first access (the daemon is still inside mount(2)). */
static bool uknl_fuse_mount(Tracee *tracee)
{
	char fstype[64];
	word_t fa = peek_reg(tracee, CURRENT, SYSARG_3);
	if (!fa || read_string(tracee, fstype, fa, sizeof fstype) <= 0) return false;
	if (strncmp(fstype, "fuse", 4) != 0) return false;

	char opts[512];
	word_t oa = peek_reg(tracee, CURRENT, SYSARG_5);
	opts[0] = '\0';
	if (oa) read_string(tracee, opts, oa, sizeof opts);
	int fdN = -1;
	const char *p = strstr(opts, "fd=");
	if (p) fdN = (int) strtol(p + 3, NULL, 10);
	if (fdN < 0) return false;

	char tgt[PATH_MAX];
	if (get_sysarg_path(tracee, tgt, SYSARG_2) < 0) return false;

	struct ukfch *ch = fch_resolve(tracee, fdN);
	if (!ch || !ch->eng) { char l[160]; snprintf(l, sizeof l, "uk_fs: fuse mount fd=%d '%s' no channel\n", fdN, tgt); uk_dbg(tracee, l); return false; }
	if (ukm_register_fuse(tgt, ch->eng, (int)(ch - g_fch)) < 0) return false;
	{ char l[PATH_MAX + 64]; snprintf(l, sizeof l, "uk_fs: FUSE mount '%s' fd=%d\n", tgt, fdN); uk_dbg(tracee, l); }
	return true;
}

/* Pool of ukfsd daemon sockets (FsBridge launches one ukfsd per name). ukfsd is
 * single-mount per process, so each live mount (a USB partition OR a loop image)
 * claims one free daemon. */
static const char *UK_POOL[] = {
	"io.neoterm.fs", "io.neoterm.fs.p1", "io.neoterm.fs.p2", "io.neoterm.fs.p3",
	"io.neoterm.fs.p4", "io.neoterm.fs.p5", "io.neoterm.fs.p6", "io.neoterm.fs.p7"
};
#define UK_NPOOL ((int)(sizeof(UK_POOL)/sizeof(UK_POOL[0])))

static int  ukfs_block_present(void);   /* fwd (defined below) */

/* Pick a daemon socket not already claimed by a live mount, or NULL if all busy. */
static const char *ukfs_alloc_daemon(void)
{
	for (int p = 0; p < UK_NPOOL; p++) {
		int busy = 0;
		for (int i = 0; i < UK_MAXM; i++) if (g_m[i].used && strcmp(g_m[i].fsname, UK_POOL[p]) == 0) { busy = 1; break; }
		if (!busy) return UK_POOL[p];
	}
	return NULL;
}

/* Parse a mount source: if it names our virtual block device or one of its
 * partitions, fill token ("uksd0"/"uksd0pN") + fsname (its io.neoterm.fs[.pN]
 * socket) and return 1; else 0. */
static int ukfs_dev_token(const char *src, char *token, size_t tcap, char *fsname, size_t fcap)
{
	const char *b = strrchr(src, '/'); b = b ? b + 1 : src;
	if (strcmp(b, "uksd0") == 0) {
		snprintf(token, tcap, "uksd0"); snprintf(fsname, fcap, "io.neoterm.fs"); return 1;
	}
	if (strncmp(b, "uksd0p", 6) == 0 && b[6]) {
		for (const char *p = b + 6; *p; p++) if (*p < '0' || *p > '9') return 0;
		snprintf(token, tcap, "%s", b);                 /* "uksd0p2" */
		snprintf(fsname, fcap, "io.neoterm.fs.%s", b + 5); /* "io.neoterm.fs.p2" */
		return 1;
	}
	return 0;
}
/* True iff src names our block device or a partition of it. */
static int ukfs_src_is_dev(const char *p)
{ char t[64], f[64]; return ukfs_dev_token(p, t, sizeof t, f, sizeof f); }

/* Drop the active mount's connection. The mount lives in the connection, so the
 * ukfsd-side mount is gone too -> clear ready (next access re-mounts). The slot
 * stays registered (the guest still has it mounted at its vmount). */
static void ukfs_sdrop(void)
{ if (g_am < 0 || g_am >= UK_MAXM) return; struct ukm *m = &g_m[g_am]; if (m->sock >= 0) { close(m->sock); m->sock = -1; } m->ready = 0; }

/* Free a mount slot entirely (real guest umount, or the backing device vanished). */
static void ukfs_unmount_slot(int i)
{
	if (i < 0 || i >= UK_MAXM || !g_m[i].used) return;
	/* FUSE-backed mount: tear down the fused engine + free its in-proot channel
	 * (req/rep buffers). The channel's guest fd is the daemon's; we don't own it. */
	if (g_m[i].fuse) {
		fused_destroy(g_m[i].fuse);
		int ci = g_m[i].fuse_chan;
		if (ci >= 0 && ci < UK_NFCH && g_fch[ci].used && g_fch[ci].eng == g_m[i].fuse) {
			/* fused_destroy sent FUSE_DESTROY; the daemon has looped back into a
			 * blocking read() and re-parked. Hand it EOF so libfuse exits and the
			 * server process dies instead of lingering ptrace-stopped. */
			fch_resume_eof(&g_fch[ci]);
			for (int k = 0; k < UK_FQ; k++) { free(g_fch[ci].rq[k].buf); free(g_fch[ci].rp[k].buf); }
			memset(&g_fch[ci], 0, sizeof g_fch[ci]);
			fch_forget_forks(ci);
		}
		memset(&g_m[i], 0, sizeof g_m[i]); g_m[i].sock = -1;
		if (g_nm > 0) g_nm--;
		return;
	}
	if (g_m[i].ready && g_m[i].sock >= 0) {
		char line[32];
		if (uksd_wn(g_m[i].sock, "UMOUNT\n", 7) >= 0) uksd_rl(g_m[i].sock, line, sizeof line);
	}
	if (g_m[i].sock >= 0) close(g_m[i].sock);
	memset(&g_m[i], 0, sizeof g_m[i]); g_m[i].sock = -1;
	if (g_nm > 0) g_nm--;
}

/* Send MOUNT for the active mount on its already-open connection. */
static int ukfs_do_mount(void)
{
	struct ukm *m = &g_m[g_am];
	if (m->sock < 0) return -1;
	/* NB: send the FULL command INCLUDING the trailing '\n' — ukfsd's read_line()
	 * blocks until it sees the newline. The explicit-range form (loop / partition of
	 * a file) carries the byte offset+size; the plain form lets ukfsd resolve a
	 * uksd0/uksd0pN token against the USB device's own partition table. */
	char req[700]; int n;
	if (m->range) n = snprintf(req, sizeof req, "MOUNT auto %s %lld %lld\n", m->token, m->base, m->size);
	else          n = snprintf(req, sizeof req, "MOUNT auto %s\n", m->token);
	if (uksd_wn(m->sock, req, n) < 0) {
		char l[96]; snprintf(l, sizeof l, "uk_fs: MOUNT %s write failed errno=%d\n", m->token, errno);
		uk_dbg_line(l); return -1;
	}
	char line[64];
	if (uksd_rl(m->sock, line, sizeof line) < 0) { uk_dbg_line("uk_fs: MOUNT no reply\n"); return -1; }
	if (line[0] == 'O' && line[1] == 'K') { char l[80]; snprintf(l, sizeof l, "uk_fs: MOUNT %s OK\n", m->token); uk_dbg_line(l); return 0; }
	{ char l[128]; snprintf(l, sizeof l, "uk_fs: MOUNT %s rejected: '%s'\n", m->token, line); uk_dbg_line(l); }
	return -1;
}

/* Connect to the active mount's ukfsd and ensure it has MOUNTed. Centralises
 * connect + lazy MOUNT so every op just calls ukfs_conn() after g_am is set. The
 * MOUNT is deferred to first access (not the mount(2) hook): on Android mount(2)
 * is SIGSYS-blocked and socket I/O from that context wouldn't reach ukfsd. */
static int ukfs_conn(void)
{
	if (g_am < 0 || g_am >= UK_MAXM || !g_m[g_am].used) return -1;
	struct ukm *m = &g_m[g_am];
	if (m->sock >= 0 && m->ready) return m->sock;
	if (m->sock < 0) {
		/* USB-backed mount and the backing device gone? auto-free the slot so the
		 * mount point reverts to the empty host dir instead of erroring forever
		 * (probed only when reconnecting). Loop mounts back a host FILE that is
		 * always present, so skip the io.neoterm.block probe for them. */
		if (m->token[0] != '/' && !ukfs_block_present()) { uk_dbg_line("uk_fs: device gone -> auto-umount\n"); ukfs_unmount_slot(g_am); g_am = -1; return -1; }
		int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (s < 0) return -1;
		struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
		setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
		struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX;
		a.sun_path[0] = '\0'; strncpy(a.sun_path + 1, m->fsname, sizeof(a.sun_path) - 2);
		socklen_t len = sizeof(a.sun_family) + 1 + strlen(m->fsname);
		if (connect(s, (struct sockaddr *) &a, len) < 0) {
			char l[96]; snprintf(l, sizeof l, "uk_fs: connect %s FAILED errno=%d\n", m->fsname, errno);
			uk_dbg_line(l); close(s); return -1;
		}
		m->sock = s;
		{ char l[80]; snprintf(l, sizeof l, "uk_fs: connected %s fd=%d (%s)\n", m->fsname, s, m->token); uk_dbg_line(l); }
	}
	if (!m->ready) {
		if (ukfs_do_mount() != 0) { ukfs_sdrop(); return -1; }
		m->ready = 1;
	}
	return m->sock;
}

/* Map a guest path to the matching mount + its mount-relative form ("/mnt/usb/a"
 * -> "/a", the mount point itself -> "/"). On a match sets g_am and returns 1;
 * else returns 0. The longest matching vmount wins (nested mount points). */
static int ukfs_rel(const char *guest, char *out, size_t osz)
{
	int best = -1; size_t bestlen = 0;
	for (int i = 0; i < UK_MAXM; i++) {
		if (!g_m[i].used) continue;
		size_t ml = strlen(g_m[i].vmount);
		if (strncmp(guest, g_m[i].vmount, ml) != 0) continue;
		char c = guest[ml];
		if (c != '\0' && c != '/') continue;            /* "/mnt/usb" vs "/mnt/usbX" */
		if (ml >= bestlen) { bestlen = ml; best = i; }
	}
	if (best < 0) return 0;
	g_am = best;
	const char *rest = guest + bestlen;
	snprintf(out, osz, "%s", rest[0] ? rest : "/");
	return 1;
}

/* Core: register (or reuse the same-vmount) slot. token is the MOUNT token
 * (uksd0/uksd0pN or a host image path); base/size/range describe an explicit byte
 * range (loop / partition of a file). Allocates a free daemon from the pool. Sets
 * g_am. Returns the slot index, or -1 (table or daemon pool full). */
static int ukm_register(const char *tgt, const char *token, long long base, long long size, int range)
{
	int slot = -1;
	for (int i = 0; i < UK_MAXM; i++) if (g_m[i].used && strcmp(g_m[i].vmount, tgt) == 0) { slot = i; break; }
	int reuse = (slot >= 0);
	if (slot < 0) for (int i = 0; i < UK_MAXM; i++) if (!g_m[i].used) { slot = i; break; }
	if (slot < 0) return -1;                          /* mount table full */
	const char *fsname;
	if (reuse) fsname = g_m[slot].fsname;             /* keep this vmount's daemon */
	else { fsname = ukfs_alloc_daemon(); if (!fsname) return -1; }  /* daemon pool full */
	if (!reuse) g_nm++;
	memset(&g_m[slot], 0, sizeof g_m[slot]);
	g_m[slot].used = 1; g_m[slot].sock = -1; g_m[slot].ready = 0;
	g_m[slot].base = base; g_m[slot].size = size; g_m[slot].range = range;
	snprintf(g_m[slot].vmount, sizeof g_m[slot].vmount, "%s", tgt);
	snprintf(g_m[slot].token,  sizeof g_m[slot].token,  "%s", token);
	snprintf(g_m[slot].fsname, sizeof g_m[slot].fsname, "%s", fsname);
	g_am = slot;
	return slot;
}

/* Register a mount slot for a guest mount of USB device <src> at <tgt>. */
static int ukfs_mount_register(const char *src, const char *tgt)
{
	char token[64], fsname[64];
	if (!ukfs_dev_token(src, token, sizeof token, fsname, sizeof fsname)) return -1;
	return ukm_register(tgt, token, 0, 0, 0);         /* token form: ukfsd resolves the partition */
}

/* Free the mount slot whose vmount == the umount target. Returns 1 if found.
 * The raw target may be RELATIVE: fusermount(1) chdir()s to the mountpoint's
 * parent and umount2()s the basename (e.g. "fmnt"), so resolve it against the
 * tracee's guest cwd and canonicalise before matching the registered vmount
 * (an absolute guest path). Without this a FUSE unmount never matched and the
 * daemon was left mounted + parked, hanging proot at shutdown. */
static int ukfs_umount_target(Tracee *tracee, const char *raw)
{
	char abs[PATH_MAX], canon[PATH_MAX];
	if (raw[0] == '/') {
		snprintf(abs, sizeof abs, "%s", raw);
	} else {
		const char *cwd = (tracee && tracee->fs && tracee->fs->cwd) ? tracee->fs->cwd : "/";
		snprintf(abs, sizeof abs, "%s/%s", cwd, raw);
	}
	ukfs_canon(abs, canon, sizeof canon);
	for (int i = 0; i < UK_MAXM; i++)
		if (g_m[i].used && (strcmp(g_m[i].vmount, canon) == 0 || strcmp(g_m[i].vmount, raw) == 0)) {
			ukfs_unmount_slot(i); if (g_am == i) g_am = -1; return 1;
		}
	return 0;
}

/* ===== loop devices (losetup / mount -o loop) =====
 * A guest sees /dev/loop-control + /dev/loopN (bound markers). losetup/mount use
 * the kernel loop ioctls to bind an image FILE to a loop device at an offset; the
 * Android kernel has no usable loop layer (no root), so we emulate the ioctls here
 * and back each loop with the image's HOST path. mount(/dev/loopN[pM]) then routes
 * to ukfsd as a file-backed mount at the loop offset (+ partition offset). This is
 * what makes `losetup -fP img; mount /dev/loop0p2 …` and `mount -o loop[,offset=N]
 * img …` behave exactly like on a real PC. */
#define UK_NLOOP 8
struct uklo { int used; char img[600]; long long off, sizelimit; int partscan; };
static struct uklo g_lo[UK_NLOOP];
static int g_lo_any = 0;        /* fast-path: any loop currently configured? */

/* Connect to an arbitrary ukfsd daemon socket (one-shot queries). fd or -1. */
static int ukfs_dconn(const char *fsname)
{
	int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (s < 0) return -1;
	struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX;
	a.sun_path[0] = '\0'; strncpy(a.sun_path + 1, fsname, sizeof(a.sun_path) - 2);
	socklen_t len = sizeof(a.sun_family) + 1 + strlen(fsname);
	if (connect(s, (struct sockaddr *) &a, len) < 0) { close(s); return -1; }
	return s;
}

/* Query the partition table of a host image file via a FREE daemon (PARTS doesn't
 * mount, so the daemon stays free). Fills start/size (bytes) for 1-based partition
 * `want`; returns 1 on success, 0 if not found. */
static int ukfs_image_part(const char *hostimg, int want, long long *start, long long *size)
{
	const char *fsname = ukfs_alloc_daemon();
	if (!fsname) fsname = UK_POOL[0];
	int s = ukfs_dconn(fsname);
	if (s < 0) return 0;
	char req[700]; int n = snprintf(req, sizeof req, "PARTS %s\n", hostimg);
	int found = 0;
	if (uksd_wn(s, req, n) >= 0) {
		char line[64];
		if (uksd_rl(s, line, sizeof line) >= 0 && line[0] == 'O') {
			int cnt = 0; if (sscanf(line, "OK %d", &cnt) == 1) {
				for (int i = 0; i < cnt; i++) {
					char pl[96]; if (uksd_rl(s, pl, sizeof pl) < 0) break;
					unsigned idx = 0, type = 0; unsigned long long st = 0, sz = 0;
					if (sscanf(pl, "p%u 0x%x %llu %llu", &idx, &type, &st, &sz) >= 3 && (int) idx == want) {
						*start = (long long) st; *size = (long long) sz; found = 1;
					}
				}
			}
		}
	}
	close(s);
	return found;
}

/* Parse "/dev/loop3" -> (n=3,part=0); "/dev/loop3p2" -> (n=3,part=2); else n=-1. */
static int ukfs_loop_parse(const char *path, int *part)
{
	const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
	if (strncmp(b, "loop", 4) != 0) return -1;
	const char *d = b + 4; if (*d < '0' || *d > '9') return -1;
	int n = 0; while (*d >= '0' && *d <= '9') n = n * 10 + (*d++ - '0');
	int pp = 0;
	if (*d == 'p') { d++; if (*d < '0' || *d > '9') return -1; while (*d >= '0' && *d <= '9') pp = pp * 10 + (*d++ - '0'); }
	if (*d) return -1;
	*part = pp; return n;
}

/* For a mount source /dev/loopN[pM]: resolve the backing host image + byte range.
 * Returns 1 (img/base/size filled) or 0 (not a configured loop / unknown part). */
static int ukfs_loop_resolve(const char *src, char *img, size_t icap, long long *base, long long *size)
{
	int part; int n = ukfs_loop_parse(src, &part);
	if (n < 0 || n >= UK_NLOOP || !g_lo[n].used || !g_lo[n].img[0]) return 0;
	snprintf(img, icap, "%s", g_lo[n].img);
	if (part == 0) { *base = g_lo[n].off; *size = g_lo[n].sizelimit; return 1; }
	long long ps = 0, pz = 0;
	if (!ukfs_image_part(g_lo[n].img, part, &ps, &pz)) return 0;
	*base = g_lo[n].off + ps; *size = pz;
	return 1;
}

/* The kernel loop ioctls we emulate (linux/loop.h). */
#define UK_LOOP_SET_FD        0x4C00
#define UK_LOOP_CLR_FD        0x4C01
#define UK_LOOP_SET_STATUS    0x4C02
#define UK_LOOP_SET_STATUS64  0x4C04
#define UK_LOOP_GET_STATUS64  0x4C05
#define UK_LOOP_SET_CAPACITY  0x4C07
#define UK_LOOP_SET_DIRECT_IO 0x4C08
#define UK_LOOP_SET_BLOCK_SIZE 0x4C09
#define UK_LOOP_CONFIGURE     0x4C0A
#define UK_LOOP_CTL_ADD       0x4C80
#define UK_LOOP_CTL_REMOVE    0x4C81
#define UK_LOOP_CTL_GET_FREE  0x4C82
#define UK_LO_FLAGS_PARTSCAN  8

/* Resolve the host image path behind a guest fd (the image losetup/mount passed). */
static int ukfs_fd_hostpath(Tracee *tracee, int fd, char *out, size_t cap)
{
	char lk[64]; snprintf(lk, sizeof lk, "/proc/%d/fd/%d", tracee->pid, fd);
	ssize_t n = readlink(lk, out, cap - 1);
	if (n <= 0) return 0;
	out[n] = '\0';
	return 1;
}

/* Emulate losetup/mount's loop ioctls on /dev/loop-control and /dev/loopN (bound
 * markers). Returns true if fully handled (result poked + PR_void). The loop
 * binding is recorded in g_lo[]; the actual FS mount happens later when the guest
 * mounts /dev/loopN[pM] (routed to ukfsd via the image host path + offset). */
static bool ukfs_loop_ioctl(Tracee *tracee)
{
	unsigned long cmd = (unsigned long) peek_reg(tracee, CURRENT, SYSARG_2);
	if ((cmd & 0xFF00) != 0x4C00) return false;   /* not a loop ioctl (cheap gate: skips TCGETS etc.) */
	int fd = (int) peek_reg(tracee, CURRENT, SYSARG_1);
	word_t arg = peek_reg(tracee, CURRENT, SYSARG_3);
	char pp[PATH_MAX];
	if (!ukfs_fd_hostpath(tracee, fd, pp, sizeof pp)) return false;
	const char *b = strrchr(pp, '/'); b = b ? b + 1 : pp;

	if (strcmp(b, "loop-control") == 0) {
		if (cmd == UK_LOOP_CTL_GET_FREE) {
			for (int i = 0; i < UK_NLOOP; i++) if (!g_lo[i].used) {
				memset(&g_lo[i], 0, sizeof g_lo[i]); g_lo[i].used = 1;   /* reserve */
				poke_reg(tracee, SYSARG_RESULT, (word_t) i); set_sysnum(tracee, PR_void); return true;
			}
			poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - ENODEV); set_sysnum(tracee, PR_void); return true;
		}
		if (cmd == UK_LOOP_CTL_ADD) {
			int i = (int) arg;
			if (i >= 0 && i < UK_NLOOP) { memset(&g_lo[i], 0, sizeof g_lo[i]); g_lo[i].used = 1; poke_reg(tracee, SYSARG_RESULT, (word_t) i); }
			else poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - EINVAL);
			set_sysnum(tracee, PR_void); return true;
		}
		if (cmd == UK_LOOP_CTL_REMOVE) {
			int i = (int) arg;
			if (i >= 0 && i < UK_NLOOP) g_lo[i].used = 0;
			poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true;
		}
		return false;
	}

	int dummy; int n = ukfs_loop_parse(b, &dummy);
	if (n < 0 || dummy != 0 || n >= UK_NLOOP) return false;   /* not a whole-loop node */
	struct uklo *L = &g_lo[n];

	switch (cmd) {
	case UK_LOOP_SET_FD: {
		char ip[PATH_MAX];
		if (!ukfs_fd_hostpath(tracee, (int) arg, ip, sizeof ip)) { poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - EBADF); set_sysnum(tracee, PR_void); return true; }
		L->used = 1; L->off = 0; L->sizelimit = 0; L->partscan = 0;
		snprintf(L->img, sizeof L->img, "%s", ip); g_lo_any = 1;
		{ char l[PATH_MAX + 64]; snprintf(l, sizeof l, "uk_fs: LOOP_SET_FD loop%d <- %s\n", n, ip); uk_dbg_line(l); }
		poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true;
	}
	case UK_LOOP_CONFIGURE: {
		unsigned char cfg[72];                              /* fd@0, info: off@32, szl@40, flags@60 */
		if (read_data(tracee, cfg, arg, sizeof cfg) < 0) { poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - EFAULT); set_sysnum(tracee, PR_void); return true; }
		unsigned int imgfd; memcpy(&imgfd, cfg + 0, 4);
		unsigned long long off, szl; unsigned int flags;
		memcpy(&off, cfg + 32, 8); memcpy(&szl, cfg + 40, 8); memcpy(&flags, cfg + 60, 4);
		char ip[PATH_MAX];
		if (!ukfs_fd_hostpath(tracee, (int) imgfd, ip, sizeof ip)) { poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - EBADF); set_sysnum(tracee, PR_void); return true; }
		L->used = 1; g_lo_any = 1; snprintf(L->img, sizeof L->img, "%s", ip);
		L->off = (long long) off; L->sizelimit = (long long) szl; L->partscan = (flags & UK_LO_FLAGS_PARTSCAN) ? 1 : 0;
		{ char l[PATH_MAX + 96]; snprintf(l, sizeof l, "uk_fs: LOOP_CONFIGURE loop%d <- %s off=%llu szl=%llu ps=%d\n", n, ip, off, szl, L->partscan); uk_dbg_line(l); }
		poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true;
	}
	case UK_LOOP_SET_STATUS:
	case UK_LOOP_SET_STATUS64: {
		unsigned char info[56];                             /* off@24, szl@32, flags@52 */
		if (read_data(tracee, info, arg, sizeof info) < 0) { poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - EFAULT); set_sysnum(tracee, PR_void); return true; }
		unsigned long long off, szl; unsigned int flags;
		memcpy(&off, info + 24, 8); memcpy(&szl, info + 32, 8); memcpy(&flags, info + 52, 4);
		L->off = (long long) off; L->sizelimit = (long long) szl;
		if (flags & UK_LO_FLAGS_PARTSCAN) L->partscan = 1;
		poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true;
	}
	case UK_LOOP_GET_STATUS64: {
		unsigned char info[232]; memset(info, 0, sizeof info);
		unsigned long long off = (unsigned long long) L->off, szl = (unsigned long long) L->sizelimit;
		unsigned int num = (unsigned) n;
		memcpy(info + 24, &off, 8); memcpy(info + 32, &szl, 8); memcpy(info + 40, &num, 4);
		snprintf((char *) info + 56, 64, "%s", L->img);     /* lo_file_name */
		if (write_data(tracee, arg, info, sizeof info) < 0) { poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - EFAULT); set_sysnum(tracee, PR_void); return true; }
		poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true;
	}
	case UK_LOOP_CLR_FD:
		L->used = 0; L->img[0] = '\0';
		poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true;
	case UK_LOOP_SET_CAPACITY:
	case UK_LOOP_SET_DIRECT_IO:
	case UK_LOOP_SET_BLOCK_SIZE:
		poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true;
	default:
		return false;
	}
}

/* Register a mount for any source we own: a USB device (/dev/uksd0[pN]) or a
 * configured loop device (/dev/loopN[pM]). Returns the slot index, or -1. */
static int ukfs_register_any(const char *src, const char *tgt)
{
	if (ukfs_src_is_dev(src)) return ukfs_mount_register(src, tgt);
	char img[600]; long long base = 0, size = 0;
	if (ukfs_loop_resolve(src, img, sizeof img, &base, &size)) {
		int r = ukm_register(tgt, img, base, size, 1);
		if (r >= 0) { char l[PATH_MAX + 96]; snprintf(l, sizeof l, "uk_fs: loop mount %s -> %s base=%lld size=%lld\n", src, img, base, size); uk_dbg_line(l); }
		return r;
	}
	return -1;
}
/* True iff src is something ukfs_register_any can mount (USB or configured loop). */
static int ukfs_src_mountable(const char *src)
{
	if (ukfs_src_is_dev(src)) return 1;
	char img[600]; long long b, s;
	return ukfs_loop_resolve(src, img, sizeof img, &b, &s);
}

/* Effective byte size of a configured loop node (whole or partition), or -1 if the
 * path isn't a configured loop. Used to present the node as a block device. */
static long long ukfs_loop_node_size(const char *gpath)
{
	char img[600]; long long base = 0, size = 0;
	if (!ukfs_loop_resolve(gpath, img, sizeof img, &base, &size)) return -1;
	if (size > 0) return size;
	struct stat ist; if (stat(img, &ist) == 0) return (long long) ist.st_size - base;
	return -1;
}

/* ===== raw I/O on a /dev/loopN[pM] node =====
 * So blkid/file/fdisk/dd and bare `mount` (auto fstype) work on a loop device just
 * like a real PC: reads/writes of the loop node are served from the backing host
 * image at the loop's byte range. Tracked per open fd. */
struct uklofd { int used, pid, fd, hostfd; long long base, size, cur; };
static struct uklofd g_lofd[64];

static struct uklofd *uklofd_find(int pid, int fd)
{
	for (int i = 0; i < 64; i++) if (g_lofd[i].used && g_lofd[i].pid == pid && g_lofd[i].fd == fd) return &g_lofd[i];
	return NULL;
}
/* If `gpath` is a configured loop node, open its backing image and remember the
 * fd→(image range) mapping so subsequent raw I/O is served from the image. */
static void uklofd_record(Tracee *tracee, int fd, const char *gpath)
{
	if (!g_lo_any) return;
	int part; if (ukfs_loop_parse(gpath, &part) < 0) return;
	char img[600]; long long base = 0, size = 0;
	if (!ukfs_loop_resolve(gpath, img, sizeof img, &base, &size)) return;
	int hf = open(img, O_RDWR | O_CLOEXEC); if (hf < 0) hf = open(img, O_RDONLY | O_CLOEXEC);
	if (hf < 0) return;
	int pid = ukfs_tgid(tracee->pid);
	struct uklofd *e = uklofd_find(pid, fd); if (e) { close(e->hostfd); }
	else { for (int i = 0; i < 64; i++) if (!g_lofd[i].used) { e = &g_lofd[i]; break; } }
	if (!e) { close(hf); return; }
	e->used = 1; e->pid = pid; e->fd = fd; e->hostfd = hf; e->base = base; e->size = size; e->cur = 0;
	/* Make the bound marker REPORT the loop's size: tools (blkid/dd/fdisk/`mount`
	 * auto-detect) fstat the node and read only up to st_size — a 0-byte marker
	 * reads as empty. Size it to the loop range (sizelimit, or image_size - base);
	 * raw reads are then served from the image by ukfs_loopfd_op. */
	long long marklen = size;
	if (marklen <= 0) { struct stat ist; if (fstat(hf, &ist) == 0) marklen = (long long) ist.st_size - base; }
	if (marklen > 0) { int mf = open(gpath, O_RDWR); if (mf >= 0) { (void) ftruncate(mf, (off_t) marklen); close(mf); } e->size = marklen; }
}
/* Serve read/pread/write/pwrite/lseek/close on a loop-node fd. Returns true if handled. */
static bool ukfs_loopfd_op(Tracee *tracee, word_t nr)
{
	if (!g_lo_any) return false;
	if (nr != PR_read && nr != PR_pread64 && nr != PR_write && nr != PR_pwrite64 &&
	    nr != PR_lseek && nr != PR_close && nr != PR_ioctl) return false;
	int pid = ukfs_tgid(tracee->pid);
	int fd = (int) peek_reg(tracee, CURRENT, SYSARG_1);
	struct uklofd *e = uklofd_find(pid, fd);
	if (!e) return false;
	long long avail = e->size;   /* sizelimit; 0 = to end of image */
	if (nr == PR_close) { close(e->hostfd); e->used = 0; return false; }  /* free, but let real close run */
	if (nr == PR_ioctl) {
		/* a loop node is a BLOCK device: mkfs/fdisk/blkid query its geometry via
		 * BLK* ioctls (not fstat). Answer from the loop range. */
		unsigned long cmd = (unsigned long) peek_reg(tracee, CURRENT, SYSARG_2);
		word_t arg = peek_reg(tracee, CURRENT, SYSARG_3);
		if (cmd == 0x80081272UL) { unsigned long long v = (unsigned long long) avail; write_data(tracee, arg, &v, 8); poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true; } /* BLKGETSIZE64 */
		if (cmd == 0x1260UL)     { unsigned long v = (unsigned long)(avail / 512); write_data(tracee, arg, &v, sizeof v); poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true; } /* BLKGETSIZE */
		if (cmd == 0x1268UL || cmd == 0x127BUL || cmd == 0x1278UL) { int v = 512; write_data(tracee, arg, &v, sizeof v); poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true; } /* BLKSSZGET / BLKPBSZGET / BLKIOMIN */
		if (cmd == 0x1279UL || cmd == 0x127AUL) { int v = 0; write_data(tracee, arg, &v, sizeof v); poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true; } /* BLKIOOPT / BLKALIGNOFF (silence mkfs "device geometry" warning) */
		if (cmd == 0x125FUL || cmd == 0x1261UL || cmd == 0x127CUL) { poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true; } /* BLKRRPART / BLKFLSBUF / BLKDISCARD-no-op */
		return false;   /* other ioctls: let them run natively */
	}
	if (nr == PR_lseek) {
		long long off = (long long)(off_t) peek_reg(tracee, CURRENT, SYSARG_2);
		int whence = (int) peek_reg(tracee, CURRENT, SYSARG_3);
		long long np = (whence == 1) ? e->cur + off : (whence == 2 && avail > 0) ? avail + off : off;
		if (np < 0) { poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - 22); set_sysnum(tracee, PR_void); return true; }
		e->cur = np; poke_reg(tracee, SYSARG_RESULT, (word_t) np); set_sysnum(tracee, PR_void); return true;
	}
	int isread = (nr == PR_read || nr == PR_pread64);
	long long pos = (nr == PR_pread64 || nr == PR_pwrite64) ? (long long)(off_t) peek_reg(tracee, CURRENT, SYSARG_4) : e->cur;
	size_t len = (size_t) peek_reg(tracee, CURRENT, SYSARG_3);
	word_t ubuf = peek_reg(tracee, CURRENT, SYSARG_2);
	if (avail > 0 && pos + (long long) len > avail) len = (pos < avail) ? (size_t)(avail - pos) : 0;
	if (len > (8u << 20)) len = 8u << 20;
	if (len == 0) { poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true; }
	void *tmp = malloc(len); if (!tmp) return false;
	long r;
	if (isread) {
		r = pread(e->hostfd, tmp, len, (off_t)(e->base + pos));
		if (r > 0) write_data(tracee, ubuf, tmp, (size_t) r);
	} else {
		if (read_data(tracee, tmp, ubuf, len) < 0) { free(tmp); poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - 14); set_sysnum(tracee, PR_void); return true; }
		r = pwrite(e->hostfd, tmp, len, (off_t)(e->base + pos));
	}
	free(tmp);
	if (r < 0) { poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - 5); set_sysnum(tracee, PR_void); return true; }
	if (nr == PR_read || nr == PR_write) e->cur = pos + r;
	poke_reg(tracee, SYSARG_RESULT, (word_t)(long) r); set_sysnum(tracee, PR_void); return true;
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

/* The deferred ukfsd MOUNT is now handled inside ukfs_conn() for the active mount
 * (connect + block-present probe + MOUNT, all lazy on first access). Kept as a thin
 * wrapper for the active mount so existing callers stay valid. */
static void ukfs_ensure_mounted(void)
{
	if (g_am < 0) return;
	(void) ukfs_conn();
}

/* STAT one path. Returns 0 (filled), -2 (ENOENT from ukfsd), -1 (socket error). */
static int ukfs_query_stat(const char *rel, unsigned *mode, unsigned *uid, unsigned *gid,
	long *size, unsigned long *ino, long *mtime, long *atime, unsigned *nlink,
	unsigned long *rdev, unsigned long *blocks)
{
	fused_t *fz = fuse_active();
	if (fz) {
		struct stat st;
		int rc = fused_getattr(fz, rel, &st);
		if (rc == -ENOENT) return -2;
		if (rc) return -1;
		*mode = st.st_mode; *uid = st.st_uid; *gid = st.st_gid;
		*size = (long) st.st_size; *ino = st.st_ino;
		*mtime = (long) st.st_mtime; *atime = (long) st.st_atime;
		*nlink = st.st_nlink; *rdev = st.st_rdev; *blocks = (unsigned long) st.st_blocks;
		return 0;
	}
	int s = ukfs_conn(); if (s < 0) return -1;
	char req[PATH_MAX + 16];
	int n = snprintf(req, sizeof req, "STAT %s\n", rel);
	if (uksd_wn(s, req, n) < 0) { ukfs_sdrop(); return -1; }
	char line[256];
	if (uksd_rl(s, line, sizeof line) < 0) { ukfs_sdrop(); return -1; }
	if (sscanf(line, "OK %u %u %u %ld %lu %ld %ld %u %lu %lu",
	           mode, uid, gid, size, ino, mtime, atime, nlink, rdev, blocks) != 10) {
		return -2;
	}
	return 0;
}

/* Whole-FS statfs of the active mount (df / statvfs). Fills the values from the
 * driver's s_op->statfs via ukfsd. 0 on success. */
static int ukfs_query_statfs(unsigned long *bsize, unsigned long long *blocks,
	unsigned long long *bfree, unsigned long long *bavail, unsigned long long *files,
	unsigned long long *ffree, long *namelen, long *frsize, long *ftype)
{
	fused_t *fz = fuse_active();
	if (fz) {
		struct statvfs sv;
		if (fused_statfs(fz, "/", &sv) != 0) return -1;
		*bsize = sv.f_bsize; *blocks = sv.f_blocks; *bfree = sv.f_bfree; *bavail = sv.f_bavail;
		*files = sv.f_files; *ffree = sv.f_ffree; *namelen = (long) sv.f_namemax;
		*frsize = (long) sv.f_frsize; *ftype = 0x65735546L /* FUSE_SUPER_MAGIC */;
		return 0;
	}
	int s = ukfs_conn(); if (s < 0) return -1;
	if (uksd_wn(s, "STATFS\n", 7) < 0) { ukfs_sdrop(); return -1; }
	char line[224];
	if (uksd_rl(s, line, sizeof line) < 0) { ukfs_sdrop(); return -1; }
	return (sscanf(line, "OK %lu %llu %llu %llu %llu %llu %ld %ld %ld",
	               bsize, blocks, bfree, bavail, files, ffree, namelen, frsize, ftype) == 9) ? 0 : -1;
}

/* Fill the guest struct statfs/statfs64 (120 B on 64-bit; same layout for both on
 * aarch64 and x86_64). f_type@0 f_bsize@8 f_blocks@16 f_bfree@24 f_bavail@32
 * f_files@40 f_ffree@48 f_namelen@64 f_frsize@72. */
static void ukfs_put_statfs(Tracee *tracee, word_t addr, unsigned long bsize,
	unsigned long long blocks, unsigned long long bfree, unsigned long long bavail,
	unsigned long long files, unsigned long long ffree, long namelen, long frsize, long ftype)
{
	unsigned char sf[120]; memset(sf, 0, sizeof sf);
	long f_type = ftype, f_bsize = (long) bsize, f_namelen = namelen ? namelen : 255, f_frsize = frsize ? frsize : (long) bsize;
	memcpy(sf + 0,  &f_type,   8); memcpy(sf + 8,  &f_bsize, 8);
	memcpy(sf + 16, &blocks,   8); memcpy(sf + 24, &bfree,   8); memcpy(sf + 32, &bavail, 8);
	memcpy(sf + 40, &files,    8); memcpy(sf + 48, &ffree,   8);
	memcpy(sf + 64, &f_namelen,8); memcpy(sf + 72, &f_frsize,8);
	write_data(tracee, addr, sf, sizeof sf);
}

/* aarch64 struct stat (128 B) — offsets match the block proxy's uksd_put_stat,
 * plus uid/gid (24/28) and a/m/ctime seconds (72/88/104). */
static void ukfs_put_stat(Tracee *tracee, word_t addr, unsigned mode, unsigned uid, unsigned gid,
	long size, unsigned long ino, long mtime, long atime, unsigned nlink,
	unsigned long rdev, unsigned long blocks)
{
	unsigned char st[144]; memset(st, 0, sizeof st);
	unsigned long st_ino = ino, st_rdev = rdev;
	unsigned int  st_mode = mode, st_nlink = nlink ? nlink : 1, st_uid = uid, st_gid = gid;
	long st_size = size, st_blocks = (long) blocks;
	long at_s = atime, mt_s = mtime, ct_s = mtime;
#if defined(__x86_64__)
	/* x86_64 struct stat (144 B): st_nlink(8)@16, st_mode@24, uid@28, gid@32,
	 * rdev@40, size@48, blksize(8)@56, blocks(8)@64, a/m/ctime@72/88/104. Only
	 * used by the host integration test build; aarch64 keeps its own layout. */
	unsigned long st_nlink8 = st_nlink, st_blksize8 = 512;
	memcpy(st + 8,   &st_ino,    8);
	memcpy(st + 16,  &st_nlink8, 8);
	memcpy(st + 24,  &st_mode,   4);
	memcpy(st + 28,  &st_uid,    4);
	memcpy(st + 32,  &st_gid,    4);
	memcpy(st + 40,  &st_rdev,   8);
	memcpy(st + 48,  &st_size,   8);
	memcpy(st + 56,  &st_blksize8, 8);
	memcpy(st + 64,  &st_blocks, 8);
	memcpy(st + 72,  &at_s,      8);
	memcpy(st + 88,  &mt_s,      8);
	memcpy(st + 104, &ct_s,      8);
#else
	/* aarch64 struct stat (128 B) — offsets match the block proxy's uksd_put_stat. */
	int st_blksize = 512;
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
#endif
	/* CRITICAL: write EXACTLY the guest struct stat size. aarch64's struct stat is
	 * 128 B; writing the full 144 B buffer overflows the guest's (often stack-
	 * allocated) struct by 16 B -> "stack smashing detected" in cat/df/ls -l. Only
	 * x86_64 (the host test build) actually uses the 144 B layout. */
#if defined(__x86_64__)
	write_data(tracee, addr, st, 144);
#else
	write_data(tracee, addr, st, 128);
#endif
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
	fused_t *fz = fuse_active();
	if (fz) { int n = fused_pread(fz, rel, buf, len, (off_t) off); return n < 0 ? -1 : n; }
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
	fused_t *fz = fuse_active();
	if (fz) {
		char tmp[PATH_MAX];
		int rc = fused_readlink(fz, rel, tmp, sizeof tmp);
		if (rc) return -1;
		size_t n = strnlen(tmp, sizeof tmp);
		if (n > bufsz) n = bufsz;
		memcpy(buf, tmp, n);
		return (long) n;
	}
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
	fused_t *fz = fuse_active();
	if (fz) {
		uint64_t fh = 0;
		int rc = fused_create(fz, rel, O_WRONLY | O_CREAT | O_TRUNC, mode, &fh);
		if (rc == 0) { fused_release(fz, rel, fh); return 0; }
		return rc;   /* already a negative errno */
	}
	int s = ukfs_conn(); if (s < 0) return -1;
	char req[PATH_MAX + 32];
	int rl = snprintf(req, sizeof req, "CREATE %u %s\n", mode, rel);
	if (uksd_wn(s, req, rl) < 0) { ukfs_sdrop(); return -1; }
	char line[32]; if (uksd_rl(s, line, sizeof line) < 0) { ukfs_sdrop(); return -1; }
	if (line[0] == 'O' && line[1] == 'K') return 0;
	{ char l[PATH_MAX + 96]; snprintf(l, sizeof l, "uk_fs: CREATE '%s' FAIL reply='%s'\n", rel, line); uk_dbg_line(l); }
	/* return the real -errno so the caller can poke it (e.g. ENOENT when the parent
	 * fan-out dir doesn't exist -> git mkdir's it and retries; -EIO would be fatal). */
	int e; return (sscanf(line, "ERR %d", &e) == 1) ? -e : -1;
}

/* WRITE <off> <len> <path> + payload: bytes written, or -1. */
static long ukfs_write_at(const char *rel, long long off, const void *buf, size_t len)
{
	fused_t *fz = fuse_active();
	if (fz) { int n = fused_pwrite(fz, rel, buf, len, (off_t) off); return n < 0 ? -1 : n; }
	int s = ukfs_conn(); if (s < 0) return -1;
	char req[PATH_MAX + 48];
	int rl = snprintf(req, sizeof req, "WRITE %lld %zu %s\n", off, len, rel);
	if (uksd_wn(s, req, rl) < 0 || uksd_wn(s, buf, len) < 0) { ukfs_sdrop(); return -1; }
	char line[64]; if (uksd_rl(s, line, sizeof line) < 0) { ukfs_sdrop(); return -1; }
	long n; if (sscanf(line, "OK %ld", &n) != 1) {
		char l[PATH_MAX + 96]; snprintf(l, sizeof l, "uk_fs: WRITE '%s' FAIL reply='%s'\n", rel, line); uk_dbg_line(l);
		return -1;
	}
	return n;
}

/* "<prefix><path>" ops (prefix carries the command + any leading numeric args
 * and a trailing space, e.g. "MKDIR 511 "). Returns 0, or -errno from ukfsd. */
/* Translate a ukfsd "<CMD> [args] " prefix op to the matching fused call. The
 * recognised CMDs cover the write-path ops the dispatch issues via ukfs_simple;
 * cosmetic/unknown ones (SYNC, perms FUSE may not honour) return 0 best-effort
 * so a FUSE mount never fails an op the kernel would have quietly accepted. */
static int fuse_simple_op(fused_t *fz, const char *prefix, const char *rel)
{
	unsigned a = 0, b = 0; long long sz = 0;
	if (!strncmp(prefix, "MKDIR ", 6))    { sscanf(prefix + 6, "%u", &a); return fused_mkdir(fz, rel, a ? a : 0755); }
	if (!strncmp(prefix, "RMDIR", 5))     return fused_rmdir(fz, rel);
	if (!strncmp(prefix, "UNLINK", 6))    return fused_unlink(fz, rel);
	if (!strncmp(prefix, "TRUNCATE ", 9)) { sscanf(prefix + 9, "%lld", &sz); return fused_truncate(fz, rel, (off_t) sz); }
	if (!strncmp(prefix, "CHMOD ", 6))    { sscanf(prefix + 6, "%u", &a); return fused_chmod(fz, rel, a); }
	if (!strncmp(prefix, "CHOWN ", 6))    { sscanf(prefix + 6, "%u %u", &a, &b); return fused_chown(fz, rel, a, b); }
	return 0;   /* SYNC / perms / anything else: best-effort success */
}

static int ukfs_simple(const char *prefix, const char *rel)
{
	fused_t *fz = fuse_active();
	if (fz) return fuse_simple_op(fz, prefix, rel);
	int s = ukfs_conn(); if (s < 0) return -1;
	char req[PATH_MAX + 64];
	int rl = snprintf(req, sizeof req, "%s%s\n", prefix, rel);
	if (uksd_wn(s, req, rl) < 0) { ukfs_sdrop(); return -1; }
	char line[64]; if (uksd_rl(s, line, sizeof line) < 0) { ukfs_sdrop(); return -1; }
	if (line[0] == 'O' && line[1] == 'K') return 0;
	{ char l[PATH_MAX + 128]; snprintf(l, sizeof l, "uk_fs: '%s%s' FAIL reply='%s'\n", prefix, rel, line); uk_dbg_line(l); }
	int e; return (sscanf(line, "ERR %d", &e) == 1) ? -e : -1;
}

/* Two byte-counted paths in the payload (RENAME old/new, SYMLINK target/link). */
static int ukfs_two_path(const char *cmd, const char *a, const char *b)
{
	fused_t *fz = fuse_active();
	if (fz) {
		if (!strcmp(cmd, "RENAME"))  return fused_rename(fz, a, b);
		if (!strcmp(cmd, "SYMLINK")) return fused_symlink(fz, a, b);  /* a=target, b=link */
		return -EINVAL;
	}
	int s = ukfs_conn(); if (s < 0) return -1;
	size_t al = strlen(a), bl = strlen(b);
	char req[64]; int rl = snprintf(req, sizeof req, "%s %zu %zu\n", cmd, al, bl);
	if (uksd_wn(s, req, rl) < 0 || uksd_wn(s, a, al) < 0 || uksd_wn(s, b, bl) < 0) { ukfs_sdrop(); return -1; }
	char line[64]; if (uksd_rl(s, line, sizeof line) < 0) { ukfs_sdrop(); return -1; }
	if (line[0] == 'O' && line[1] == 'K') return 0;
	{ char l[2 * PATH_MAX + 96]; snprintf(l, sizeof l, "uk_fs: %s FAIL reply='%s' a='%s' b='%s'\n", cmd, line, a, b); uk_dbg_line(l); }
	int e; return (sscanf(line, "ERR %d", &e) == 1) ? -e : -1;
}

/* Thread-group id of a tracee. Under ptrace, tracee->pid is the THREAD id (TID);
 * file descriptors are shared by the whole thread group, so the vfd table must be
 * keyed by the tgid — otherwise a worker thread (e.g. git index-pack's delta
 * resolvers) reading/mmap'ing a fd that the leader opened wouldn't find the vfd,
 * and its read would hit the empty placeholder ("premature end of pack file").
 * The tid->tgid map is stable for a thread's lifetime, so cache it. */
static struct { int tid, tgid; } g_tgid_cache[512];
static int g_tgid_cn = 0;
static int ukfs_tgid(int tid)
{
	for (int i = 0; i < g_tgid_cn; i++) if (g_tgid_cache[i].tid == tid) return g_tgid_cache[i].tgid;
	int tgid = tid;
	char path[64]; snprintf(path, sizeof path, "/proc/%d/status", tid);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		char buf[2048]; ssize_t n = read(fd, buf, sizeof buf - 1); close(fd);
		if (n > 0) { buf[n] = '\0'; char *t = strstr(buf, "Tgid:"); if (t) tgid = atoi(t + 5); }
	}
	if (g_tgid_cn < 512) { g_tgid_cache[g_tgid_cn].tid = tid; g_tgid_cache[g_tgid_cn].tgid = tgid; g_tgid_cn++; }
	return tgid;
}
/* parent pid of @tid (from /proc/<tid>/status PPid). Read fresh (not cached): used
 * only on the lazy inheritance walk, which runs once per process. */
static int ukfs_ppid(int tid)
{
	int ppid = 0;
	char path[64]; snprintf(path, sizeof path, "/proc/%d/status", tid);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		char buf[2048]; ssize_t n = read(fd, buf, sizeof buf - 1); close(fd);
		if (n > 0) { buf[n] = '\0'; char *t = strstr(buf, "PPid:"); if (t) ppid = atoi(t + 5); }
	}
	return ppid;
}

/* ---- vfd table: a guest fd (over a placeholder file) -> a ukfsd path. The
 *      guest opens a real placeholder (/dev/null or /), gets a real fd, and
 *      every read/lseek/getdents/close on it is proxied here. ---- */
struct ukfs_dent { unsigned long long ino; int type; char name[256]; };
struct ukfs_vfd {
	int used, pid, fd, isdir, wrote;   /* wrote: needs a SYNC on close (deferred flush) */
	int mnt;                        /* which mount slot (g_m[]) this fd belongs to */
	int append;                     /* O_APPEND: each write goes to EOF (offset re-evaluated) */
	int mmapw;                      /* has a PROT_WRITE|MAP_SHARED mapping -> flush placeholder back */
	long long off;                  /* regular-file read cursor */
	char path[PATH_MAX];            /* vmount-relative */
	char backing[64];               /* per-fd placeholder guest path (for mmap populate/cleanup) */
	struct ukfs_dent *dents;        /* dir: parsed LIST incl. "." and ".." */
	int dent_n, dent_idx;           /* count and emit cursor */
};
struct ukfs_pending { int used, pid, isdir, append, mnt; char path[PATH_MAX]; char backing[64]; };
static struct ukfs_vfd     g_vfd[128];
static struct ukfs_pending g_open_pending[64];
static int                 g_ph_seq;   /* unique per-open placeholder id */

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

/* A traced process exited. Reap its vfds and loop fds: a short-lived process
 * (e.g. rpi-imager spawning lsblk over and over for drive polling) just exits
 * without close()ing its open library fds — the kernel closes them, so the
 * redirect never sees the close and the vfd slot leaks. Enough rapid forks then
 * exhaust the 128-slot table; further opens get no vfd and their reads fall
 * through to the empty placeholder, so the dynamic loader reports "file too
 * short". Freeing on exit keeps the table from filling. Called per tracee exit. */
void uknl_fs_pid_gone(int pid);
void uknl_fs_pid_gone(int pid)
{
	for (int i = 0; i < 128; i++)
		if (g_vfd[i].used && g_vfd[i].pid == pid) vfd_free(&g_vfd[i]);
	for (int i = 0; i < 64; i++)
		if (g_lofd[i].used && g_lofd[i].pid == pid) { close(g_lofd[i].hostfd); memset(&g_lofd[i], 0, sizeof g_lofd[i]); }
}

/* Lazy fd-inheritance: a forked child inherits a COPY of the parent's fd table, but
 * our vfds are keyed by tgid, so the child's tgid starts empty. proot tracks
 * fork/clone via ptrace EVENTS (not translate_syscall_exit), so we can't copy at
 * fork time. Instead, the FIRST time a tgid does any fd op, walk up the process
 * ancestry and copy the nearest ancestor's vfds into this tgid — capturing exactly
 * the inherited fds (classically the shell's `cmd > file`: the shell opens the file,
 * dup2s it onto fd 1, then forks the external cmd). Race-free (driven by the child's
 * own op) and O(1) afterwards. */
static int g_inh_tgid[512];
static int g_inh_n = 0;
static int inh_mark(int tg)   /* returns 1 if already resolved, else records + returns 0 */
{
	for (int i = 0; i < g_inh_n; i++) if (g_inh_tgid[i] == tg) return 1;
	/* RECYCLE when full instead of permanently returning "resolved" — a long
	 * fork-heavy session (git clone forks dozens of processes) would otherwise fill
	 * the table and then SKIP inheritance for every new forked child, so its writes to
	 * an inherited vmount fd (`yes | head > file`) hit the empty placeholder and the
	 * file reads back as zeros. Re-walks after a recycle are made safe by the
	 * duplicate guard in vfd_lookup (it won't re-copy an fd the child already has). */
	if (g_inh_n >= 512) g_inh_n = 0;
	g_inh_tgid[g_inh_n++] = tg;
	return 0;
}
static struct ukfs_vfd *vfd_lookup(Tracee *tracee, int fd)
{
	int tg = ukfs_tgid(tracee->pid);
	struct ukfs_vfd *v = vfd_find(tg, fd);
	if (v) { g_am = v->mnt; return v; }       /* route subsequent ukfsd I/O to this fd's mount */
	if (inh_mark(tg)) return NULL;            /* ancestry already resolved for this tgid */
	int tid = tracee->pid;
	for (int hop = 0; hop < 16; hop++) {       /* walk to the nearest ancestor with vfds */
		int ppid = ukfs_ppid(tid);
		if (ppid <= 1) break;
		int ptg = ukfs_tgid(ppid);
		if (ptg != tg) {
			int idx[128], ni = 0;
			for (int i = 0; i < 128 && ni < 128; i++)
				if (g_vfd[i].used && g_vfd[i].pid == ptg) idx[ni++] = i;
			if (ni) {
				for (int k = 0; k < ni; k++) {
					if (vfd_find(tg, g_vfd[idx[k]].fd)) continue;   /* already have it (recycled re-walk) */
					struct ukfs_vfd *nv = vfd_alloc();
					if (!nv) break;
					*nv = g_vfd[idx[k]]; nv->pid = tg;
					nv->dents = NULL; nv->dent_n = nv->dent_idx = 0;
				}
				break;                     /* nearest ancestor with vfds wins */
			}
		}
		tid = ppid;
	}
	v = vfd_find(tg, fd);
	if (v) g_am = v->mnt;                      /* route to the (now-inherited) fd's mount */
	return v;
}

/* Pending-result table: for path syscalls I PR_void at sysENTER (renameat,
 * mkdirat, unlinkat, …), proot's PR_void result-restoration does NOT reliably
 * deliver the poked result to the tracee on aarch64 — the value is clobbered
 * before the tracee resumes (rename poke=0 was observed arriving at git as -1).
 * fd-based ops (read/write/stat) are unaffected (they are FILTER_SYSEXIT). So we
 * record the intended result here at enter and authoritatively RE-POKE it from
 * uknl_fs_exit_final, which runs at the very end of translate_syscall_exit (after
 * fake_id0's SYSCALL_EXIT_END). One syscall is in flight per tracee, so a single
 * slot per pid suffices. */
static struct { int used, pid; word_t nr; long res; } g_pend_res[128];
static void pend_res_set(int pid, word_t nr, long res)
{
	int i, fr = -1;
	for (i = 0; i < 128; i++) {
		if (g_pend_res[i].used) { if (g_pend_res[i].pid == pid) { g_pend_res[i].nr = nr; g_pend_res[i].res = res; return; } }
		else if (fr < 0) fr = i;
	}
	if (fr >= 0) { g_pend_res[fr].used = 1; g_pend_res[fr].pid = pid; g_pend_res[fr].nr = nr; g_pend_res[fr].res = res; }
}
static int pend_res_take(int pid, word_t nr, long *res)
{
	for (int i = 0; i < 128; i++)
		if (g_pend_res[i].used && g_pend_res[i].pid == pid && g_pend_res[i].nr == nr) {
			*res = g_pend_res[i].res; g_pend_res[i].used = 0; return 1;
		}
	return 0;
}

static void open_pending_set(int pid, int isdir, int append, const char *path, const char *backing)
{
	int i;
	for (i = 0; i < 64; i++) if (g_open_pending[i].used && g_open_pending[i].pid == pid) break;
	if (i == 64) for (i = 0; i < 64; i++) if (!g_open_pending[i].used) break;
	if (i == 64) return;
	g_open_pending[i].used = 1; g_open_pending[i].pid = pid; g_open_pending[i].isdir = isdir;
	g_open_pending[i].append = append;
	g_open_pending[i].mnt = g_am;   /* the open path was just resolved to this mount */
	snprintf(g_open_pending[i].path, sizeof g_open_pending[i].path, "%s", path);
	snprintf(g_open_pending[i].backing, sizeof g_open_pending[i].backing, "%s", backing ? backing : "");
}

/* Lazily LIST the directory and parse it (with synthetic "." / "..") on the
 * first getdents64. blob format: count x {u8 type, u64 ino, u64 size, u16
 * namelen, name[]} (little-endian), matching ukfsd's LIST reply. */
/* fused readdir -> v->dents builder (mirrors the ukfsd LIST path below). */
struct fuse_dir_ctx { struct ukfs_vfd *v; size_t cap; int oom; };
static void fuse_dir_emit(void *c, const char *name, unsigned type)
{
	struct fuse_dir_ctx *x = c; struct ukfs_vfd *v = x->v;
	if (x->oom) return;
	if (!strcmp(name, ".") || !strcmp(name, "..")) return;   /* we add these ourselves */
	if ((size_t) v->dent_n >= x->cap) {
		size_t nc = x->cap ? x->cap * 2 : 16;
		void *p = realloc(v->dents, nc * sizeof(struct ukfs_dent));
		if (!p) { x->oom = 1; return; }
		v->dents = p; x->cap = nc;
	}
	struct ukfs_dent *d = &v->dents[v->dent_n++];
	memset(d, 0, sizeof *d);
	snprintf(d->name, sizeof d->name, "%s", name);
	/* fused passes a DT_* type; ukfs_dent uses 1=dir, 2=reg (see ukfs_emit_dents). */
	d->type = (type == 4 /*DT_DIR*/) ? 1 : (type == 8 /*DT_REG*/) ? 2 : 0;
}

static int ukfs_load_dir(struct ukfs_vfd *v)
{
	g_am = v->mnt;                             /* route to this dir's mount */
	fused_t *fz = fuse_active();
	if (fz) {
		v->dents = calloc(2, sizeof(struct ukfs_dent));
		if (!v->dents) return -1;
		v->dents[0].type = 1; strcpy(v->dents[0].name, ".");
		v->dents[1].type = 1; strcpy(v->dents[1].name, "..");
		v->dent_n = 2;
		struct fuse_dir_ctx ctx = { v, 2, 0 };
		int rc = fused_readdir(fz, v->path, fuse_dir_emit, &ctx);
		if (rc || ctx.oom) { free(v->dents); v->dents = NULL; v->dent_n = 0; return -1; }
		v->dent_idx = 0;
		return 0;
	}
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
/* Verbose/info tracing — emitted only when UK_FS_DEBUG is set, so the default
 * dmesg stays clean. Error/failure paths keep using uk_dbg_line/uk_dbg directly. */
static int uk_verbose(void)
{
	static int v = -1;
	if (v < 0) { const char *e = getenv("UK_FS_DEBUG"); v = (e && *e && *e != '0') ? 1 : 0; }
	return v;
}
static void uk_dbgv_line(const char *line) { if (uk_verbose()) uk_dbg_line(line); }
static void uk_dbgv(Tracee *tracee, const char *line) { if (uk_verbose()) uk_dbg(tracee, line); }

/* Mount hook, called from apply_emulated_mount() — the common point for BOTH
 * the normal mount(2) trap AND the SIGSYS path Android uses to block mount(2)
 * (which bypasses translate_syscall_enter, hence the redirect's own PR_mount
 * branch never runs on-device). Returns true if it handled a /dev/uksd0 mount:
 * tells ukfsd to MOUNT and records the vmount; the caller then reports success
 * to the guest. */
static bool uknl_fs_mount_hook(Tracee *tracee)
{
	if (!uk_fs_on()) return false;
	/* FUSE mount (fstype "fuse*") — matched by fstype/options, before the src check. */
	if (uknl_fuse_mount(tracee)) return true;
	char src[PATH_MAX];
	if (get_sysarg_path(tracee, src, SYSARG_1) < 0) return false;
	if (!ukfs_src_mountable(src)) return false;
	char tgt[PATH_MAX];
	if (get_sysarg_path(tracee, tgt, SYSARG_2) < 0) return false;
	/* Register the vmount slot only — no socket I/O here (this may run in the SIGSYS
	 * context where it wouldn't deliver). The ukfsd MOUNT happens lazily on the
	 * first access under the mount point. The src (uksd0 / uksd0pN / loopN[pM])
	 * selects which partition/image + which ukfsd this mount talks to. */
	if (ukfs_register_any(src, tgt) < 0) return false;
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
	if (!uk_fs_on() || g_nm == 0) return false;
	char tgt[PATH_MAX];
	if (get_sysarg_path(tracee, tgt, SYSARG_1) < 0) return false;
	char l[PATH_MAX + 64];
	snprintf(l, sizeof l, "uk_fs: UMOUNT hook tgt='%s'\n", tgt);
	uk_dbg(tracee, l);
	return ukfs_umount_target(tracee, tgt) ? true : false;   /* not our mount point -> let proot handle */
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
	struct ukfs_vfd *v = vfd_lookup(tracee, dirfd);
	if (!v) return 0;                       /* dirfd isn't one of our mount fds */
	g_am = v->mnt;                          /* the dirfd's mount selects the target's mount */
	if (!path[0])
		snprintf(rel, rsz, "%s", v->path);              /* empty path => the dir itself */
	else if (v->path[0] == '/' && v->path[1] == '\0')
		snprintf(rel, rsz, "/%s", path);                /* dir is the mount root */
	else
		snprintf(rel, rsz, "%s/%s", v->path, path);
	return 1;
}

/* Shared openat/openat2 redirect for a vmount-relative path @rel with open
 * @flags. Return codes: 1 = fully handled (SYSARG_RESULT poked + PR_void);
 * 0 = rewrote to the FILE placeholder, caller must force O_CREAT so it
 * self-creates; 2 = rewrote to the DIR placeholder ("/"); -1 = not intercepted.
 * The file placeholder is a REAL regular file (/.ukfs_ph), not /dev/null, so that
 * fd-based metadata ops the guest does on it (utimensat/fchmod/fsetxattr/ioctl)
 * succeed instead of EPERMing on the root-owned /dev/null device. */
/* @patharg is the register holding the path to rewrite to the placeholder:
 * SYSARG_2 for openat/openat2, SYSARG_1 for the legacy creat(2). */
static int ukfs_open_redirect_arg(Tracee *tracee, const char *rel, int flags, Reg patharg)
{
	unsigned mode, uid, gid, nlink; long size, mt, at; unsigned long ino, rdev, blocks;
	int q = ukfs_query_stat(rel, &mode, &uid, &gid, &size, &ino, &mt, &at, &nlink, &rdev, &blocks);
	if (q == -1) return -1;
	int isdir = (q == 0) && ((mode & 0170000) == 0040000);   /* S_IFDIR */
	if (q == -2) {
		if (flags & O_CREAT) {
			int ce = ukfs_create_at(rel, 0100644);
			if (ce < 0) {
				/* poke the real errno (ENOENT when the parent dir is missing), not a
				 * blanket EIO — git relies on ENOENT to create objects/<xx>/ and retry. */
				poke_reg(tracee, SYSARG_RESULT, (word_t)(long)(ce == -1 ? -EIO : ce)); set_sysnum(tracee, PR_void); return 1;
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
	/* Files get a UNIQUE placeholder (not a shared /.ukfs_ph): mmap maps the
	 * placeholder's real pages, so a shared empty file would SIGBUS and two mmap'd
	 * files would collide. The unique inode is populated with real content on mmap
	 * and unlinked right after open (anonymous; freed on close, no clutter). */
	char backing[64]; backing[0] = 0;
	if (isdir) {
		(void) set_sysarg_path(tracee, "/", patharg);
	} else {
		snprintf(backing, sizeof backing, "/.ukfs_ph_%d_%d", tracee->pid, ++g_ph_seq);
		(void) set_sysarg_path(tracee, backing, patharg);
	}
	open_pending_set(tracee->pid, isdir, (flags & O_APPEND) ? 1 : 0, rel, backing);
	return isdir ? 2 : 0;
}
/* openat/openat2 path lives in SYSARG_2 — the common case. */
static int ukfs_open_redirect(Tracee *tracee, const char *rel, int flags)
{ return ukfs_open_redirect_arg(tracee, rel, flags, SYSARG_2); }

/* mmap(2) on a vfd maps the placeholder inode's real pages, NOT the intercepted
 * read() data — so an empty placeholder SIGBUSes the moment the tracee touches
 * the mapping (this is what crashed git: it mmaps packfiles/index). Fill the
 * placeholder inode behind the guest fd with the file's true ukfs content (via
 * /proc/<pid>/fd/<fd>, which refers to the same anonymous inode the guest mapped)
 * and size it exactly, so the native mmap then sees real data. Streaming
 * read()/write() stay intercepted; this only materialises bytes for mmap. */
static void ukfs_populate_fd(Tracee *tracee, int fd, const char *relpath)
{
	unsigned mode, uid, gid, nlink; long size, mt, at; unsigned long ino, rdev, blocks;
	if (ukfs_query_stat(relpath, &mode, &uid, &gid, &size, &ino, &mt, &at, &nlink, &rdev, &blocks) != 0)
		return;
	char proc[64]; snprintf(proc, sizeof proc, "/proc/%d/fd/%d", tracee->pid, fd);
	int bfd = open(proc, O_WRONLY | O_CLOEXEC);
	if (bfd < 0) { char l[96]; snprintf(l, sizeof l, "uk_fs: mmap populate open(%s) errno=%d\n", proc, errno); uk_dbg_line(l); return; }
	char *buf = malloc(65536);
	long long off = 0; int failed = !buf;
	while (!failed && off < size) {
		long n = ukfs_read_at(relpath, off, buf, 65536);
		/* A FUSE backend can briefly return a short/empty read under load; retry a
		 * few times before giving up so we never materialise a truncated library
		 * (the dynamic loader would then fail with "file too short"). */
		for (int r = 0; n == 0 && r < 16 && off < size; r++) n = ukfs_read_at(relpath, off, buf, 65536);
		if (n <= 0) { failed = 1; break; }
		long w = 0;
		while (w < n) {
			ssize_t k = pwrite(bfd, buf + w, (size_t)(n - w), (off_t)(off + w));
			if (k <= 0) { failed = 1; break; }
			w += k;
		}
		if (failed) break;
		off += n;
	}
	free(buf);
	if (ftruncate(bfd, (off_t) size) != 0) failed = 1;
	if (failed || off < size) {
		char l[PATH_MAX + 128];
		snprintf(l, sizeof l, "uk_fs: mmap populate INCOMPLETE '%s' off=%lld size=%ld mode=0%o errno=%d\n",
		         relpath, off, size, mode, errno);
		uk_dbg_line(l);
	}
	close(bfd);
}

/* Writeback for a PROT_WRITE|MAP_SHARED mapping: the guest wrote into the
 * placeholder inode via the mapping (databases, mmap-based editors); read it back
 * through /proc/<pid>/fd/<fd> and push it to ukfs. Called on msync/close — by which
 * point msync/munmap has flushed the dirty pages to the placeholder inode. */
static void ukfs_flush_mmap(Tracee *tracee, struct ukfs_vfd *v, int fd)
{
	char proc[64]; snprintf(proc, sizeof proc, "/proc/%d/fd/%d", tracee->pid, fd);
	int bfd = open(proc, O_RDONLY | O_CLOEXEC);
	if (bfd < 0) return;
	off_t end = lseek(bfd, 0, SEEK_END);
	char *buf = malloc(65536);
	long long off = 0;
	if (buf && end > 0) {
		while (off < (long long) end) {
			ssize_t n = pread(bfd, buf, 65536, (off_t) off);
			if (n <= 0) break;
			if (ukfs_write_at(v->path, off, buf, (size_t) n) < 0) break;
			off += n;
		}
		v->wrote = 1;            /* ensure the deferred SYNC fires */
	}
	free(buf);
	close(bfd);
}

/* Lexical path canonicalisation: collapse "." and ".." and redundant slashes
 * (no symlink following — the guest paths here are already vmount-logical). Used
 * to turn a chdir target into a clean absolute guest path for the cwd. */
static void ukfs_canon(const char *in, char *out, size_t osz)
{
	char tmp[PATH_MAX]; snprintf(tmp, sizeof tmp, "%s", in);
	const char *parts[256]; int np = 0; char *save = 0;
	for (char *t = strtok_r(tmp, "/", &save); t; t = strtok_r(0, "/", &save)) {
		if (t[0] == '.' && t[1] == '\0') continue;
		if (t[0] == '.' && t[1] == '.' && t[2] == '\0') { if (np > 0) np--; continue; }
		if (np < 256) parts[np++] = t;
	}
	if (np == 0) { snprintf(out, osz, "/"); return; }
	size_t o = 0; out[0] = '\0';
	for (int i = 0; i < np && o + 1 < osz; i++) {
		int w = snprintf(out + o, osz - o, "/%s", parts[i]);
		if (w < 0) break;
		o += (size_t) w;
	}
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
		snprintf(l, sizeof l, "uk_fs: INIT v50-loop UK_FS='%s' UK_BLOCK='%s'\n",
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

	/* The FUSE daemon's read()/write() on its /dev/fuse channel fd -> bridge to the
	 * fused engine (before the g_nm guard: the channel exists from open, pre-mount). */
	if (uknl_fuse_chan_io(tracee, nr)) return true;

	/* open("/dev/fuse") by a guest libfuse daemon: note the thread so the exit hook
	 * captures the returned fd as a FUSE channel. NO morph — the marker open just
	 * proceeds. Before the g_nm guard: the daemon opens /dev/fuse before any mount. */
	if (nr == PR_openat || nr == PR_openat2 || nr == PR_open || nr == PR_creat) {
		int patharg = (nr == PR_open || nr == PR_creat) ? SYSARG_1 : SYSARG_2;
		word_t pa = peek_reg(tracee, CURRENT, patharg);
		char gp[PATH_MAX];
		if (pa && read_string(tracee, gp, pa, sizeof gp) > 0 && strcmp(gp, "/dev/fuse") == 0)
			uknl_fuse_open_enter(tracee);   /* fall through: let the open proceed */
	}

	/* loop device ioctls (losetup / mount -o loop) — handled before the g_nm guard
	 * since losetup runs BEFORE any filesystem is mounted. */
	if (nr == PR_ioctl && ukfs_loop_ioctl(tracee)) return true;
	/* raw read/write/lseek on a /dev/loopN[pM] node -> serve from the backing image
	 * (blkid/file/fdisk/dd and bare `mount` auto-detect). Before the g_nm guard:
	 * blkid reads the loop device before any mount exists. */
	if (ukfs_loopfd_op(tracee, nr)) return true;

	/* stat of a /dev/loopN[pM] node -> report a BLOCK device (S_IFBLK). Otherwise
	 * tools see a regular-file marker: `mount /dev/loop0p1` would try to loop-mount
	 * the marker itself instead of mounting the partition, and mkfs/losetup misjudge
	 * it. Path forms (stat/lstat/newfstatat/statx) and fstat(fd) both covered. */
	if (g_lo_any && (nr == PR_newfstatat || nr == PR_fstatat64 || nr == PR_statx ||
	                 nr == PR_stat || nr == PR_lstat || nr == PR_stat64 || nr == PR_lstat64 || nr == PR_fstat)) {
		long long lsz = -1; word_t stbuf = 0; int is_x = 0;
		if (nr == PR_fstat) {
			int fd = (int) peek_reg(tracee, CURRENT, SYSARG_1);
			char lk[64], pp[PATH_MAX]; snprintf(lk, sizeof lk, "/proc/%d/fd/%d", tracee->pid, fd);
			ssize_t pn = readlink(lk, pp, sizeof pp - 1);
			if (pn > 0) { pp[pn] = '\0'; lsz = ukfs_loop_node_size(pp); }
			stbuf = peek_reg(tracee, CURRENT, SYSARG_2);
		} else {
			int patharg = (nr == PR_stat || nr == PR_lstat || nr == PR_stat64 || nr == PR_lstat64) ? SYSARG_1 : SYSARG_2;
			word_t pa = peek_reg(tracee, CURRENT, patharg);
			char gp[PATH_MAX];
			if (pa && read_string(tracee, gp, pa, sizeof gp) > 0) lsz = ukfs_loop_node_size(gp);
			is_x = (nr == PR_statx);
			stbuf = is_x ? peek_reg(tracee, CURRENT, SYSARG_5)
			      : (nr == PR_newfstatat || nr == PR_fstatat64) ? peek_reg(tracee, CURRENT, SYSARG_3)
			      : peek_reg(tracee, CURRENT, SYSARG_2);
		}
		if (lsz >= 0) {
			/* S_IFBLK | 0660, sd-like rdev 8:0 */
			if (is_x) ukfs_put_statx(tracee, stbuf, 0060660, 0, 0, (long) lsz, 0xb10c0, 0, 0, 1, (8UL << 8), (unsigned long)(lsz / 512));
			else      ukfs_put_stat (tracee, stbuf, 0060660, 0, 0, (long) lsz, 0xb10c0, 0, 0, 1, (8UL << 8), (unsigned long)(lsz / 512));
			pend_res_set(tracee->pid, nr, 0);
			poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true;
		}
	}

	/* mount(2) on the NORMAL path (non-Android, where mount isn't SIGSYS-blocked):
	 * register the vmount and fake success. The actual ukfsd MOUNT is deferred to
	 * first access, same as the apply_emulated_mount hook used on Android. */
	if (nr == PR_mount) {
		/* FUSE mount (fstype "fuse*") first — it's matched by fstype/options, not src. */
		if (uknl_fuse_mount(tracee)) { poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true; }
		word_t sa = peek_reg(tracee, CURRENT, SYSARG_1);
		char src[PATH_MAX];
		if (!sa || read_string(tracee, src, sa, sizeof src) <= 0) return false;
		if (!ukfs_src_mountable(src)) return false;
		word_t ta = peek_reg(tracee, CURRENT, SYSARG_2);
		char tgt[PATH_MAX];
		if (!ta || read_string(tracee, tgt, ta, sizeof tgt) <= 0) return false;
		if (ukfs_register_any(src, tgt) < 0) return false;
		poke_reg(tracee, SYSARG_RESULT, 0);
		set_sysnum(tracee, PR_void);
		return true;
	}

	/* umount(2)/umount2(2) on the NORMAL path (non-Android). On Android these are
	 * SIGSYS-blocked and handled via the apply_emulated_umount hook instead. */
	if (nr == PR_umount2) {
		if (g_nm == 0) return false;
		word_t ta = peek_reg(tracee, CURRENT, SYSARG_1);
		char tgt[PATH_MAX];
		if (!ta || read_string(tracee, tgt, ta, sizeof tgt) <= 0) return false;
		if (!ukfs_umount_target(tracee, tgt)) return false;
		poke_reg(tracee, SYSARG_RESULT, 0);
		set_sysnum(tracee, PR_void);
		return true;
	}

	if (g_nm == 0) return false;
	/* MOUNT is now ensured per-op inside ukfs_conn() once g_am is resolved by the
	 * path; no global ensure here (we don't yet know which mount this op targets). */

	/* execve/execveat of a program stored ON the vmount. The kernel would read the
	 * (empty) placeholder and fail with ENOEXEC ("not found"). Materialise the ukfs
	 * file into a real rootfs temp file the kernel can actually read+exec, and rewrite
	 * the exec path to it; proot then loads the temp normally. For a #! script the
	 * interpreter re-opens this same real path, so it works too. The temp is named per
	 * pid (overwritten on the pid's next exec — bounded, no unlink so scripts can read
	 * it). Non-vmount execs are returned untouched. */
	if (nr == PR_execve || nr == PR_execveat) {
		/* A /dev/fuse opener that exec()s an app after mounting (AppImage runtime ->
		 * AppRun) owns a foreground mount; mark it so its mount is torn down when it
		 * exits (uknl_fuse_owner_gone). Daemonizing FUSE (sshfs) never exec()s here. */
		uknl_fuse_note_exec(ukfs_tgid(tracee->pid));
		int patharg = (nr == PR_execveat) ? SYSARG_2 : SYSARG_1;
		int dirfd = (nr == PR_execveat) ? (int) peek_reg(tracee, CURRENT, SYSARG_1) : AT_FDCWD;
		word_t pa = peek_reg(tracee, CURRENT, patharg);
		char gp[PATH_MAX], rel[PATH_MAX];
		if (!pa || read_string(tracee, gp, pa, sizeof gp) <= 0) return false;
		if (!ukfs_rel_at(tracee, dirfd, gp, rel, sizeof rel)) return false;   /* not under vmount */
		unsigned mode, uid, gid, nlink; long size, mt, at; unsigned long ino, rdev, blocks;
		if (ukfs_query_stat(rel, &mode, &uid, &gid, &size, &ino, &mt, &at, &nlink, &rdev, &blocks) != 0) return false;
		if ((mode & 0170000) != 0100000) return false;   /* only regular files */

		/* #! script on the vmount: don't materialise (that would make the kernel exec
		 * a temp path, so the script sees $0 = /.ukfs_exec_* and self-locating tools
		 * like AppImage AppRun compute the wrong $APPDIR). Instead rewrite execve to
		 * run the INTERPRETER with the original mount path as argv[1]: the interpreter
		 * open()s that path -> served via the FS redirect -> and $0 is the real mount
		 * path. */
		{
			char hdr[256];
			long hn = ukfs_read_at(rel, 0, hdr, sizeof hdr - 1);
			if (hn >= 2 && hdr[0] == '#' && hdr[1] == '!') {
				char line[256]; int li = 0;
				for (long i = 2; i < hn && hdr[i] != '\n' && li < (int) sizeof line - 1; i++) line[li++] = hdr[i];
				line[li] = '\0';
				char *q = line; while (*q == ' ' || *q == '\t') q++;
				char interp[256], iarg[256]; iarg[0] = '\0';
				int k = 0; while (*q && *q != ' ' && *q != '\t' && k < (int) sizeof interp - 1) interp[k++] = *q++;
				interp[k] = '\0';
				while (*q == ' ' || *q == '\t') q++;
				if (*q) snprintf(iarg, sizeof iarg, "%s", q);
				int argvarg = (nr == PR_execveat) ? SYSARG_3 : SYSARG_2;
				word_t oargv = peek_reg(tracee, CURRENT, argvarg);
				word_t optr[250]; int on = 0, oerr = 0;
				if (oargv) for (; on < 250; on++) {
					word_t w = 0;
					if (read_data(tracee, &w, oargv + (word_t) on * sizeof(word_t), sizeof w) < 0) { oerr = 1; break; }
					if (w == 0) break;
					optr[on] = w;
				}
				if (interp[0] == '/' && !oerr) {
					word_t s_i = alloc_mem(tracee, (ssize_t)(strlen(interp) + 1));
					word_t s_p = alloc_mem(tracee, (ssize_t)(strlen(gp) + 1));
					word_t s_a = iarg[0] ? alloc_mem(tracee, (ssize_t)(strlen(iarg) + 1)) : 1;
					word_t s_v = alloc_mem(tracee, (ssize_t)((on + 4) * sizeof(word_t)));
					if (s_i && s_p && s_a && s_v) {
						write_data(tracee, s_i, interp, strlen(interp) + 1);
						write_data(tracee, s_p, gp, strlen(gp) + 1);
						if (iarg[0]) write_data(tracee, s_a, iarg, strlen(iarg) + 1);
						word_t arr[256]; int ai = 0;
						arr[ai++] = s_i;
						if (iarg[0]) arr[ai++] = s_a;
						arr[ai++] = s_p;                              /* argv[1] = script path = $0 */
						for (int i = 1; i < on; i++) arr[ai++] = optr[i];   /* original args after argv[0] */
						arr[ai++] = 0;
						write_data(tracee, s_v, arr, (word_t) ai * sizeof(word_t));
						set_sysarg_path(tracee, interp, patharg);     /* exec the interpreter */
						poke_reg(tracee, argvarg, s_v);
						char l[PATH_MAX + 64]; snprintf(l, sizeof l, "uk_fs: exec script %s via %s ($0 preserved)\n", gp, interp); uk_dbg(tracee, l);
						return false;
					}
				}
				/* shebang rewrite not possible: fall through to materialise */
			}
		}
		char gtmp[64]; snprintf(gtmp, sizeof gtmp, "/.ukfs_exec_%d", tracee->pid);
		char htmp[PATH_MAX];
		if (translate_path(tracee, htmp, AT_FDCWD, gtmp, false) < 0) return false;
		int bfd = open(htmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
		if (bfd < 0) return false;
		char *buf = malloc(65536); long long off = 0; int ok = 1;
		if (buf) {
			while (off < size) {
				long n = ukfs_read_at(rel, off, buf, 65536);
				if (n <= 0) break;
				if (write(bfd, buf, (size_t) n) != (ssize_t) n) { ok = 0; break; }
				off += n;
			}
			free(buf);
		} else ok = 0;
		(void) fchmod(bfd, 0755);
		close(bfd);
		if (!ok || off < size) { (void) unlink(htmp); return false; }   /* materialise failed: let it ENOEXEC */
		(void) set_sysarg_path(tracee, gtmp, patharg);   /* exec the temp instead */
		return false;                                    /* let proot/kernel run execve */
	}

	/* chdir/fchdir INTO the vmount. proot would translate the target to the (empty)
	 * host shadow and lstat it -> ENOENT, so `cd` into the mounted FS fails and any
	 * tool that then uses RELATIVE paths (mkdir -p, configure, git, make) breaks
	 * because tracee->fs->cwd isn't a vmount path. Validate the target is a ukfs
	 * directory, set the guest cwd to its canonical vmount path ourselves so
	 * ukfs_rel_at(AT_FDCWD, …) resolves subsequent relative ops, and void the
	 * syscall (proot's exit handler forces chdir's result to 0). */
	if (nr == PR_chdir) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_1);
		char gp[PATH_MAX];
		if (!pa || read_string(tracee, gp, pa, sizeof gp) <= 0) return false;
		char abs[2 * PATH_MAX], canon[PATH_MAX], rel[PATH_MAX];
		if (gp[0] == '/') snprintf(abs, sizeof abs, "%s", gp);
		else { const char *cwd = (tracee->fs && tracee->fs->cwd) ? tracee->fs->cwd : "/"; snprintf(abs, sizeof abs, "%s/%s", cwd, gp); }
		ukfs_canon(abs, canon, sizeof canon);
		if (!ukfs_rel(canon, rel, sizeof rel)) return false;     /* not under vmount: let proot handle */
		unsigned mode, uid, gid, nlink; long size, mt, at; unsigned long ino, rdev, blocks;
		int q = ukfs_query_stat(rel, &mode, &uid, &gid, &size, &ino, &mt, &at, &nlink, &rdev, &blocks);
		if (q != 0 || (mode & 0170000) != 0040000) return false; /* not a ukfs dir: let proot fail it */
		char *tmp = talloc_strdup(tracee->fs, canon);
		if (tmp) { TALLOC_FREE(tracee->fs->cwd); tracee->fs->cwd = tmp; talloc_set_name_const(tracee->fs->cwd, "$cwd"); }
		poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_fchdir) {
		struct ukfs_vfd *v = vfd_lookup(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1));
		if (!v || !v->isdir) return false;
		g_am = v->mnt;                                  /* this fd's mount selects the vmount prefix */
		const char *vm = (g_am >= 0 && g_am < UK_MAXM) ? g_m[g_am].vmount : "";
		char canon[PATH_MAX];
		if (v->path[0] == '/' && v->path[1] == '\0') snprintf(canon, sizeof canon, "%s", vm);
		else snprintf(canon, sizeof canon, "%s%s", vm, v->path);
		char *tmp = talloc_strdup(tracee->fs, canon);
		if (tmp) { TALLOC_FREE(tracee->fs->cwd); tracee->fs->cwd = tmp; talloc_set_name_const(tracee->fs->cwd, "$cwd"); }
		poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true;
	}
	/* getcwd while the cwd is under the vmount. proot's getcwd verifies the cwd by
	 * translating "." to the host shadow and lstat'ing it — which fails (ENOENT)
	 * for a vmount SUBDIR that lives only in ukfs, so any tool that cd's into the
	 * mounted FS and calls getcwd (git, make, an interactive shell) breaks. Answer
	 * it ourselves from the tracked guest cwd. */
	if (nr == PR_getcwd) {
		const char *cwd = (tracee->fs && tracee->fs->cwd) ? tracee->fs->cwd : "/";
		char rel[PATH_MAX];
		if (!ukfs_rel(cwd, rel, sizeof rel)) return false;   /* not under vmount: proot handles */
		word_t buf = peek_reg(tracee, CURRENT, SYSARG_1);
		size_t size = (size_t) peek_reg(tracee, CURRENT, SYSARG_2);
		size_t len = strlen(cwd) + 1;
		long r = (size < len) ? -ERANGE : (long) len;
		if (r > 0 && buf) write_data(tracee, buf, cwd, len);
		pend_res_set(tracee->pid, nr, r);
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) r); set_sysnum(tracee, PR_void); return true;
	}

	/* statfs(path)/statfs64 and fstatfs(fd)/fstatfs64 under a ukfs mount: answer
	 * `df` / statvfs from the real driver stats instead of leaking the host rootfs
	 * size. statfs path is SYSARG_1; the struct (statfs/statfs64, 120 B on 64-bit)
	 * is SYSARG_2 for statfs / SYSARG_2 for fstatfs. */
	if (nr == PR_statfs || nr == PR_statfs64) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_1);
		char gp[PATH_MAX], rel[PATH_MAX];
		if (!pa || read_string(tracee, gp, pa, sizeof gp) <= 0) return false;
		if (!ukfs_rel(gp, rel, sizeof rel)) return false;     /* not under a vmount */
		unsigned long bs; unsigned long long bl, bf, ba, fi, ff; long nl, fr, ft;
		if (ukfs_query_statfs(&bs, &bl, &bf, &ba, &fi, &ff, &nl, &fr, &ft) != 0) return false;
		ukfs_put_statfs(tracee, peek_reg(tracee, CURRENT, SYSARG_2), bs, bl, bf, ba, fi, ff, nl, fr, ft);
		pend_res_set(tracee->pid, nr, 0);
		poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_fstatfs || nr == PR_fstatfs64) {
		struct ukfs_vfd *v = vfd_lookup(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1));
		if (!v) return false;                                 /* g_am set by vfd_lookup */
		unsigned long bs; unsigned long long bl, bf, ba, fi, ff; long nl, fr, ft;
		if (ukfs_query_statfs(&bs, &bl, &bf, &ba, &fi, &ff, &nl, &fr, &ft) != 0) return false;
		ukfs_put_statfs(tracee, peek_reg(tracee, CURRENT, SYSARG_2), bs, bl, bf, ba, fi, ff, nl, fr, ft);
		pend_res_set(tracee->pid, nr, 0);
		poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true;
	}

	/* mmap(2)/mmap2(2) on a vfd: materialise the real content into the placeholder
	 * inode the guest is about to map, then let the native mmap proceed. mmap fd is
	 * SYSARG_5; MAP_ANONYMOUS opens (fd=-1) and non-vfd fds fall straight through. */
	if (nr == PR_mmap || nr == PR_mmap2) {
		int fd = (int) peek_reg(tracee, CURRENT, SYSARG_5);
		struct ukfs_vfd *v = (fd >= 0) ? vfd_lookup(tracee, fd) : NULL;
		if (!v || v->isdir) return false;
		ukfs_populate_fd(tracee, fd, v->path);
		unsigned prot = (unsigned) peek_reg(tracee, CURRENT, SYSARG_3);
		unsigned flags = (unsigned) peek_reg(tracee, CURRENT, SYSARG_4);
		if ((prot & 0x2 /*PROT_WRITE*/) && (flags & 0x1 /*MAP_SHARED*/)) v->mmapw = 1;
		return false;     /* native mmap now maps real bytes */
	}
	/* msync(addr,len,flags): can't map the address back to a vfd, so flush EVERY
	 * writable-shared mapping in this thread group — the mmap-write data is otherwise
	 * stranded in the placeholder inode (databases call msync without closing). */
	if (nr == PR_msync) {
		int tg = ukfs_tgid(tracee->pid), any = 0;
		for (int i = 0; i < 128; i++)
			if (g_vfd[i].used && g_vfd[i].pid == tg && g_vfd[i].mmapw) { g_am = g_vfd[i].mnt; ukfs_flush_mmap(tracee, &g_vfd[i], g_vfd[i].fd); any = 1; }
		if (!any) return false;          /* nothing of ours mapped: let the native msync run */
		poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true;
	}

	/* fd-based ops on a vfd (set up by a prior openat). */
	if (nr == PR_read || nr == PR_pread64) {
		struct ukfs_vfd *v = vfd_lookup(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1));
		if (!v || v->isdir) return false;
		long long pos = (nr == PR_pread64)
			? (long long)(off_t) peek_reg(tracee, CURRENT, SYSARG_4) : v->off;
		size_t len = (size_t) peek_reg(tracee, CURRENT, SYSARG_3);
		word_t buf = peek_reg(tracee, CURRENT, SYSARG_2);
		if (len == 0) { poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true; }
		if (len > (8u << 20)) len = 8u << 20;
		void *tmp = malloc(len); if (!tmp) return false;
		/* Fill the whole request, looping over the backend. A single backend read
		 * tops out at the FUSE one-message cap (128 KiB) and a FUSE daemon may
		 * legitimately return a short read at a block boundary; a regular file,
		 * though, only short-reads at EOF, so naive readers (the dynamic loader ->
		 * "file too short") break on a premature short return. Loop until len or EOF. */
		long got = 0, n = 0, fsize = -1;
		while ((size_t) got < len) {
			n = ukfs_read_at(v->path, pos + got, (char *) tmp + got, len - (size_t) got);
			if (n < 0) break;
			if (n == 0) {
				/* A regular file only short-reads at real EOF; a premature 0 here
				 * (a FUSE daemon momentarily returning nothing under load) makes the
				 * dynamic loader fail with "file too short". Distinguish the two by
				 * the file's real size (queried once) and retry a transient 0. */
				if (fsize < 0) {
					unsigned m, u, g_, nl; long sz, mt, at; unsigned long in, rd, bl;
					fsize = (ukfs_query_stat(v->path, &m, &u, &g_, &sz, &in, &mt, &at, &nl, &rd, &bl) == 0) ? sz : (pos + got);
				}
				if (pos + got >= fsize) break;          /* genuine EOF */
				for (int r = 0; r < 16 && n == 0; r++)
					n = ukfs_read_at(v->path, pos + got, (char *) tmp + got, len - (size_t) got);
				if (n <= 0) break;                      /* still nothing: return partial */
			}
			got += n;
		}
		if (got > 0) write_data(tracee, buf, tmp, (size_t) got);
		free(tmp);
		if (got == 0 && n < 0) { poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - EIO); set_sysnum(tracee, PR_void); return true; }
		if (nr == PR_read) v->off += got;
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) got); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_write || nr == PR_pwrite64) {
		struct ukfs_vfd *v = vfd_lookup(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1));
		if (!v || v->isdir) return false;
		long long pos = (nr == PR_pwrite64)
			? (long long)(off_t) peek_reg(tracee, CURRENT, SYSARG_4) : v->off;
		/* O_APPEND (PR_write only; pwrite64 ignores O_APPEND per POSIX): each write
		 * atomically goes to the CURRENT end of file. The vfd cursor starts at 0 and
		 * isn't advanced by other writers, so without this an append clobbers byte 0
		 * (e.g. git rewriting .git/config via lock+append). Re-query the live size. */
		if (nr == PR_write && v->append) {
			unsigned mode, uid, gid, nlink; long size, mt, at; unsigned long ino, rdev, blocks;
			if (ukfs_query_stat(v->path, &mode, &uid, &gid, &size, &ino, &mt, &at, &nlink, &rdev, &blocks) == 0)
				pos = (long long) size;
		}
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
		if (strstr(v->path, "config")) { char l[PATH_MAX + 96]; snprintf(l, sizeof l, "uk_fs: WRITE poke=%ld off=%lld len=%zu p='%s'\n", n, pos, len, v->path); uk_dbg_line(l); }
		if (n < 0) { poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - EIO); set_sysnum(tracee, PR_void); return true; }
		/* advance the cursor: for O_APPEND to the post-write EOF (pos was the live size),
		 * otherwise from the previous cursor. pwrite64 never moves the cursor. */
		if (nr == PR_write) v->off = (v->append ? pos : v->off) + n;
		v->wrote = 1;          /* defer the block-device flush to close (SYNC) */
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) n); set_sysnum(tracee, PR_void); return true;
	}
	/* copy_file_range(fd_in,_,fd_out,_,len,_) and sendfile(out_fd,in_fd,...) move bytes
	 * directly between two kernel fds. When one side is a vmount placeholder fd, the
	 * kernel copy bypasses ukfs entirely (data goes to/from the empty placeholder) —
	 * e.g. `mv hostfile mnt/x` (rename EXDEV -> coreutils copy_file_range) lands an
	 * empty file. Report ENOSYS so libc/coreutils fall back to the read()/write() loop,
	 * which we DO intercept. (Pure host<->host copies are left to the kernel.) */
	if (nr == PR_copy_file_range || nr == PR_sendfile || nr == PR_sendfile64) {
		int in_fd, out_fd;
		if (nr == PR_copy_file_range) { in_fd = (int) peek_reg(tracee, CURRENT, SYSARG_1); out_fd = (int) peek_reg(tracee, CURRENT, SYSARG_3); }
		else { out_fd = (int) peek_reg(tracee, CURRENT, SYSARG_1); in_fd = (int) peek_reg(tracee, CURRENT, SYSARG_2); }
		if (!vfd_lookup(tracee, in_fd) && !vfd_lookup(tracee, out_fd)) return false;   /* neither side is ours */
		pend_res_set(tracee->pid, nr, -ENOSYS);
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - ENOSYS); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_ftruncate) {
		struct ukfs_vfd *v = vfd_lookup(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1));
		if (!v) return false;
		long long sz = (long long)(off_t) peek_reg(tracee, CURRENT, SYSARG_2);
		char pfx[48]; snprintf(pfx, sizeof pfx, "TRUNCATE %lld ", sz);
		int r = ukfs_simple(pfx, v->path);
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) r); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_lseek) {
		struct ukfs_vfd *v = vfd_lookup(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1));
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
		struct ukfs_vfd *v = vfd_lookup(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1));
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
		/* getdents64 is trapped by BOTH our set AND proot's hidden_files extension,
		 * so (like renameat) the PR_void-poked byte count can be clobbered before the
		 * tracee resumes -> the caller sees a short/empty dir and stops recursing
		 * (rm -rf can't descend). Re-poke the real count from uknl_fs_exit_final. */
		pend_res_set(tracee->pid, nr, (long) w);
		poke_reg(tracee, SYSARG_RESULT, (word_t) w); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_close) {
		int cfd = (int) peek_reg(tracee, CURRENT, SYSARG_1);
		struct ukfs_vfd *v = vfd_lookup(tracee, cfd);
		if (v) {
			/* push any mmap-written content back to ukfs before the placeholder dies */
			if (v->mmapw) ukfs_flush_mmap(tracee, v, cfd);
			/* Flush deferred writes once, here, instead of per write() — the latter
			 * is O(n^2) (each sync rewrites the whole dirty-buffer list). */
			if (v->wrote) {
				long sr = ukfs_simple("SYNC ", v->path);
				if (strstr(v->path, "config")) { char l[PATH_MAX + 64]; snprintf(l, sizeof l, "uk_fs: CLOSE sync=%ld p='%s'\n", sr, v->path); uk_dbg_line(l); }
			}
			vfd_free(v);
		}
		return false;          /* let the real close of the placeholder fd run */
	}
	/* fsync/fdatasync on a vfd: flush the deferred writes (data is otherwise only in
	 * ukfsd's page cache until close). The placeholder fd is /dev/null, whose fsync
	 * returns EINVAL — which makes editors report "Error writing: Invalid argument". */
	if (nr == PR_fsync || nr == PR_fdatasync) {
		struct ukfs_vfd *v = vfd_lookup(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1));
		if (!v) return false;
		if (v->wrote) { (void) ukfs_simple("SYNC ", v->path); v->wrote = 0; }
		poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true;
	}
	/* fd-based metadata on a vfd. The placeholder is /dev/null, so fchmod/fchown on
	 * it return EPERM — which breaks tools that set perms via the fd (e.g. git's
	 * adjust_shared_perm on .git/config: "could not write config file ... Operation
	 * not permitted"). Route them to the ukfs path instead. */
	if (nr == PR_fchmod) {
		struct ukfs_vfd *v = vfd_lookup(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1));
		if (!v) return false;
		unsigned mode = (unsigned) peek_reg(tracee, CURRENT, SYSARG_2) & 07777;
		char pfx[48]; snprintf(pfx, sizeof pfx, "CHMOD %u ", mode);
		(void) ukfs_simple(pfx, v->path);            /* best-effort; FAT perms are cosmetic */
		poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_fchown) {
		struct ukfs_vfd *v = vfd_lookup(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1));
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
		struct ukfs_vfd *v = vfd_lookup(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1));
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
				struct ukfs_vfd *v = vfd_lookup(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1));
				if (v) { snprintf(rel, sizeof rel, "%s", v->path); have_rel = 1; }
			}
		}
		if (!have_rel) return false;
		unsigned mode, uid, gid, nlink; long size, mt, at; unsigned long ino, rdev, blocks;
		int q = ukfs_query_stat(rel, &mode, &uid, &gid, &size, &ino, &mt, &at, &nlink, &rdev, &blocks);
		if (q == -1) return false;                 /* socket down: let host try */
		if (q == -2) {
			pend_res_set(tracee->pid, nr, -ENOENT);
			poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - ENOENT);
			set_sysnum(tracee, PR_void); return true;
		}
		if (nr == PR_statx)
			ukfs_put_statx(tracee, peek_reg(tracee, CURRENT, SYSARG_5),
			               mode, uid, gid, size, ino, mt, at, nlink, rdev, blocks);
		else
			ukfs_put_stat(tracee, peek_reg(tracee, CURRENT, SYSARG_3),
			              mode, uid, gid, size, ino, mt, at, nlink, rdev, blocks);
		/* re-poke the result from exit_final: statx/newfstatat are FILTER_SYSENTER
		 * path syscalls whose PR_void result can be clobbered before the tracee
		 * resumes (same loss as renameat) — without this, stat() of a directory can
		 * spuriously return ENOENT. The struct written above survives (guest memory). */
		pend_res_set(tracee->pid, nr, 0);
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
		if (!ukfs_rel_at(tracee, AT_FDCWD, gp, rel, sizeof rel)) return false;  /* legacy path arg: CWD-relative */
		pend_res_set(tracee->pid, nr, -ENOTSUP);
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - ENOTSUP); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_listxattr || nr == PR_llistxattr) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_1); char gp[PATH_MAX], rel[PATH_MAX];
		if (!pa || read_string(tracee, gp, pa, sizeof gp) <= 0) return false;
		if (!ukfs_rel_at(tracee, AT_FDCWD, gp, rel, sizeof rel)) return false;  /* legacy path arg: CWD-relative */
		pend_res_set(tracee->pid, nr, 0);
		poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true;   /* no xattrs */
	}
	/* set/remove xattr under the vmount: no xattrs on FAT, report success (no-op).
	 * Crucially this also stops the call reaching fake_id0, which would try to
	 * persist it on the non-existent host-shadow path and return EPERM. */
	if (nr == PR_setxattr || nr == PR_lsetxattr || nr == PR_removexattr || nr == PR_lremovexattr) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_1); char gp[PATH_MAX], rel[PATH_MAX];
		if (!pa || read_string(tracee, gp, pa, sizeof gp) <= 0) return false;
		if (!ukfs_rel_at(tracee, AT_FDCWD, gp, rel, sizeof rel)) return false;  /* legacy path arg: CWD-relative */
		pend_res_set(tracee->pid, nr, 0);
		poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true;
	}
	/* utimensat/futimesat/utimes under the vmount: actually SET the times via the
	 * engine's UTIME op (FAT stores mtime/atime), not a no-op — `make` depends on
	 * mtime ordering, and cp -p/tar/touch -d/rsync restore explicit times. Path in
	 * SYSARG_2 (utimensat/futimesat) or SYSARG_1 (utimes); an empty/NULL path means
	 * the dirfd itself (futimens). Times array: SYSARG_3 (utimensat/futimesat) or
	 * SYSARG_2 (utimes); NULL => now. nsec==-1 to the engine means "leave unchanged". */
	if (nr == PR_utimensat || nr == PR_futimesat || nr == PR_utimes) {
		word_t pa = peek_reg(tracee, CURRENT, (nr == PR_utimes) ? SYSARG_1 : SYSARG_2);
		char gp[PATH_MAX], rel[PATH_MAX];
		if (pa && read_string(tracee, gp, pa, sizeof gp) > 0 && gp[0]) {
			if (!ukfs_rel_at(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1), gp, rel, sizeof rel)) return false;
		} else {
			if (nr == PR_utimes) return false;          /* utimes needs a path */
			struct ukfs_vfd *vv = vfd_lookup(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1));
			if (!vv) return false;
			snprintf(rel, sizeof rel, "%s", vv->path);
		}
		long long asec = -1, ansec = -1, msec = -1, mnsec = -1;
		long nowt = (long) time(0);
		int is_ts = (nr == PR_utimensat);          /* timespec (nsec) vs timeval (usec) */
		word_t tp = peek_reg(tracee, CURRENT, (nr == PR_utimes) ? SYSARG_2 : SYSARG_3);
		long long t[4];
		if (!tp || read_data(tracee, t, tp, sizeof t) < 0) {
			asec = msec = nowt; ansec = mnsec = 0;     /* NULL times => now */
		} else {
			if (is_ts && t[1] == 0x3ffffffe) ansec = -1;                         /* UTIME_OMIT */
			else if (is_ts && t[1] == 0x3fffffff) { asec = nowt; ansec = 0; }    /* UTIME_NOW  */
			else { asec = t[0]; ansec = is_ts ? t[1] : t[1] * 1000; }
			if (is_ts && t[3] == 0x3ffffffe) mnsec = -1;
			else if (is_ts && t[3] == 0x3fffffff) { msec = nowt; mnsec = 0; }
			else { msec = t[2]; mnsec = is_ts ? t[3] : t[3] * 1000; }
		}
		char cmd[160]; snprintf(cmd, sizeof cmd, "UTIME %lld %lld %lld %lld ", asec, ansec, msec, mnsec);
		long r = ukfs_simple(cmd, rel);
		pend_res_set(tracee->pid, nr, r);
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) r); set_sysnum(tracee, PR_void); return true;
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
		/* legacy access(2) takes a CWD-relative path (dirfd = AT_FDCWD); resolving it
		 * with ukfs_rel (absolute-only) misses relative probes like git's
		 * access(".git/config", R_OK), which then fall through to the host, return
		 * ENOENT, and make git silently skip the repo's local config. */
		int dfd = isaccess ? AT_FDCWD : (int) peek_reg(tracee, CURRENT, SYSARG_1);
		if (!ukfs_rel_at(tracee, dfd, gp, rel, sizeof rel))
			return false;
		unsigned mode, uid, gid, nlink; long size, mt, at; unsigned long ino, rdev, blocks;
		int q = ukfs_query_stat(rel, &mode, &uid, &gid, &size, &ino, &mt, &at, &nlink, &rdev, &blocks);
		if (q == -1) return false;                 /* socket down: let host try */
		long ar = (q == 0 ? 0 : -ENOENT);
		pend_res_set(tracee->pid, nr, ar);
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) ar);
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
		if (n < 0) {
			/* distinguish "doesn't exist" (ENOENT) from "exists but not a symlink"
			 * (EINVAL): git readlinks HEAD to detect it, and EINPLACE of ENOENT makes
			 * it believe a non-existent HEAD exists -> it skips writing it -> the repo
			 * is invalid ("not in a git directory"). */
			unsigned m2, u2, g2, nl2; long sz2, mt2, at2; unsigned long i2, rd2, bl2;
			int q2 = ukfs_query_stat(rel, &m2, &u2, &g2, &sz2, &i2, &mt2, &at2, &nl2, &rd2, &bl2);
			long err = (q2 == -2) ? -ENOENT : -EINVAL;
			pend_res_set(tracee->pid, nr, err); poke_reg(tracee, SYSARG_RESULT, (word_t)(long) err); set_sysnum(tracee, PR_void); return true;
		}
		size_t bufsz = (size_t) peek_reg(tracee, CURRENT, SYSARG_4);
		word_t buf = peek_reg(tracee, CURRENT, SYSARG_3);
		if ((size_t) n > bufsz) n = (long) bufsz;
		if (n > 0) write_data(tracee, buf, tgt, (size_t) n);
		pend_res_set(tracee->pid, nr, n);
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) n); set_sysnum(tracee, PR_void); return true;
	}

	/* path-based metadata/namespace ops under the vmount. */
	if (nr == PR_mkdirat) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_2); char gp[PATH_MAX], rel[PATH_MAX];
		if (!pa || read_string(tracee, gp, pa, sizeof gp) <= 0) return false;
		int mdfd = (int) peek_reg(tracee, CURRENT, SYSARG_1);
		if (!ukfs_rel_at(tracee, mdfd, gp, rel, sizeof rel)) {
			return false;
		}
		unsigned mode = (unsigned) peek_reg(tracee, CURRENT, SYSARG_3) & 07777;
		char pfx[48]; snprintf(pfx, sizeof pfx, "MKDIR %u ", mode);
		long r = ukfs_simple(pfx, rel);
		pend_res_set(tracee->pid, nr, r);
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) r); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_unlinkat) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_2); char gp[PATH_MAX], rel[PATH_MAX];
		if (!pa || read_string(tracee, gp, pa, sizeof gp) <= 0) return false;
		if (!ukfs_rel_at(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1), gp, rel, sizeof rel)) return false;
		int flags = (int) peek_reg(tracee, CURRENT, SYSARG_3);
		long r = ukfs_simple((flags & 0x200) ? "RMDIR " : "UNLINK ", rel);   /* AT_REMOVEDIR */
		pend_res_set(tracee->pid, nr, r);
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) r); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_symlinkat) {
		word_t ta = peek_reg(tracee, CURRENT, SYSARG_1);    /* target, stored verbatim */
		word_t la = peek_reg(tracee, CURRENT, SYSARG_3);    /* link path (an FS path) */
		char target[PATH_MAX], lp[PATH_MAX], rel[PATH_MAX];
		if (!ta || read_string(tracee, target, ta, sizeof target) <= 0) return false;
		if (!la || read_string(tracee, lp, la, sizeof lp) <= 0) return false;
		if (!ukfs_rel_at(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_2), lp, rel, sizeof rel)) return false;
		long r = ukfs_two_path("SYMLINK", target, rel);
		pend_res_set(tracee->pid, nr, r);
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) r); set_sysnum(tracee, PR_void); return true;
	}
	/* fchmodat(dirfd, path, mode, flags) AND fchmodat2(dirfd, path, mode, flags):
	 * identical arg layout for our purposes (we ignore AT_SYMLINK_NOFOLLOW — FAT has
	 * no symlinks). glibc routes fchmodat(...,AT_SYMLINK_NOFOLLOW) through the newer
	 * fchmodat2 syscall, which tar/cp -p/rsync use to restore directory modes; proot
	 * is taught fchmodat2 (sysnum 452) by the patch script so it traps here too. */
	if (nr == PR_fchmodat || nr == PR_fchmodat2) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_2); char gp[PATH_MAX], rel[PATH_MAX];
		if (!pa || read_string(tracee, gp, pa, sizeof gp) <= 0) return false;
		if (!ukfs_rel_at(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1), gp, rel, sizeof rel)) return false;
		unsigned mode = (unsigned) peek_reg(tracee, CURRENT, SYSARG_3) & 07777;
		char pfx[48]; snprintf(pfx, sizeof pfx, "CHMOD %u ", mode);
		long r = ukfs_simple(pfx, rel);
		pend_res_set(tracee->pid, nr, r);
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) r); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_fchownat) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_2); char gp[PATH_MAX], rel[PATH_MAX];
		if (!pa || read_string(tracee, gp, pa, sizeof gp) <= 0) return false;
		if (!ukfs_rel_at(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1), gp, rel, sizeof rel)) return false;
		unsigned uid = (unsigned) peek_reg(tracee, CURRENT, SYSARG_3);
		unsigned gid = (unsigned) peek_reg(tracee, CURRENT, SYSARG_4);
		char pfx[64]; snprintf(pfx, sizeof pfx, "CHOWN %u %u ", uid, gid);
		long r = ukfs_simple(pfx, rel);
		pend_res_set(tracee->pid, nr, r);
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) r); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_truncate) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_1); char gp[PATH_MAX], rel[PATH_MAX];
		if (!pa || read_string(tracee, gp, pa, sizeof gp) <= 0) return false;
		if (!ukfs_rel_at(tracee, AT_FDCWD, gp, rel, sizeof rel)) return false;  /* legacy path arg: CWD-relative */
		long long sz = (long long)(off_t) peek_reg(tracee, CURRENT, SYSARG_2);
		char pfx[48]; snprintf(pfx, sizeof pfx, "TRUNCATE %lld ", sz);
		long r = ukfs_simple(pfx, rel);
		pend_res_set(tracee->pid, nr, r);
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) r); set_sysnum(tracee, PR_void); return true;
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
			uk_dbg_line("uk_fs: RENAME EXDEV (cross-boundary)\n");
			poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - EXDEV); set_sysnum(tracee, PR_void); return true;
		}
		long rr = ukfs_two_path("RENAME", orel, nrel);
		{ char l[2 * PATH_MAX + 96]; snprintf(l, sizeof l, "uk_fs: RENAME poke=%ld o='%s' n='%s'\n", rr, orel, nrel); uk_dbgv_line(l); }
		pend_res_set(tracee->pid, nr, rr);
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) rr); set_sysnum(tracee, PR_void); return true;
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
		if (r == 0) {
			/* FILE placeholder (/.ukfs_ph): force O_CREAT so it self-creates, strip
			 * O_EXCL/O_TRUNC/O_DIRECTORY (else O_EXCL would "File exists" once it
			 * exists), set a sane create mode. Real create/trunc already happened on
			 * the FS side; the placeholder just yields a usable, writable real fd. */
			poke_reg(tracee, SYSARG_3, (word_t)(unsigned)((flags & ~(O_EXCL | O_TRUNC | O_DIRECTORY)) | O_CREAT));
			poke_reg(tracee, SYSARG_4, (word_t) 0600);
		} else if (r == 2) {
			/* DIR placeholder ("/"): strip create/excl/trunc/dir-flag. */
			poke_reg(tracee, SYSARG_3, (word_t)(unsigned)(flags & ~(O_CREAT | O_EXCL | O_TRUNC | O_DIRECTORY)));
		}
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
		if (r == 0 || r == 2) {
			/* FILE placeholder (/.ukfs_ph): force O_CREAT + mode so it self-creates.
			 * DIR placeholder ("/"): just strip create/excl/trunc/dir. Clear resolve
			 * so the absolute placeholder isn't rejected by RESOLVE_BENEATH/IN_ROOT. */
			how[0] &= ~(unsigned long long)(O_CREAT | O_EXCL | O_TRUNC | O_DIRECTORY);
			if (r == 0) { how[0] |= O_CREAT; how[1] = 0600; }
			how[2] = 0;
			write_data(tracee, howp, how, n);
		}
		return r == 1;
	}

	/* ---- legacy non-*at syscalls (path in SYSARG_1). aarch64 has ONLY the *at
	 * forms, so these never fire on-device; x86_64 and 32-bit ABIs use them, so
	 * handling them makes the redirect portable and lets the host integration test
	 * exercise the real stack. Each delegates to the same ukfsd op as its *at twin
	 * with AT_FDCWD. ---- */
	/* legacy creat(path, mode) == openat(AT_FDCWD, path, O_WRONLY|O_CREAT|O_TRUNC,
	 * mode). The path is in SYSARG_1 (mode in SYSARG_2), so the placeholder rewrite
	 * targets SYSARG_1 and the unmodified SYSARG_2 mode stays. Used by tar/cpio and
	 * other tools to create output files; without it the archive lands on the host
	 * shadow and never appears in the mount. */
	if (nr == PR_creat) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_1); char gp[PATH_MAX], rel[PATH_MAX];
		if (!pa || read_string(tracee, gp, pa, sizeof gp) <= 0) return false;
		if (!ukfs_rel_at(tracee, AT_FDCWD, gp, rel, sizeof rel)) return false;
		int r = ukfs_open_redirect_arg(tracee, rel, O_WRONLY | O_CREAT | O_TRUNC, SYSARG_1);
		if (r == 2) { poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - EISDIR); set_sysnum(tracee, PR_void); return true; }
		return r == 1;   /* r==0: host creats the placeholder -> real writable fd, vfd set up at exit */
	}
	if (nr == PR_mkdir) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_1); char gp[PATH_MAX], rel[PATH_MAX];
		if (!pa || read_string(tracee, gp, pa, sizeof gp) <= 0) return false;
		if (!ukfs_rel_at(tracee, AT_FDCWD, gp, rel, sizeof rel)) return false;
		unsigned mode = (unsigned) peek_reg(tracee, CURRENT, SYSARG_2) & 07777;
		char pfx[48]; snprintf(pfx, sizeof pfx, "MKDIR %u ", mode);
		long r = ukfs_simple(pfx, rel); pend_res_set(tracee->pid, nr, r);
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) r); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_rmdir || nr == PR_unlink) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_1); char gp[PATH_MAX], rel[PATH_MAX];
		if (!pa || read_string(tracee, gp, pa, sizeof gp) <= 0) return false;
		if (!ukfs_rel_at(tracee, AT_FDCWD, gp, rel, sizeof rel)) return false;
		long r = ukfs_simple(nr == PR_rmdir ? "RMDIR " : "UNLINK ", rel); pend_res_set(tracee->pid, nr, r);
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) r); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_rename) {
		word_t oa = peek_reg(tracee, CURRENT, SYSARG_1), na = peek_reg(tracee, CURRENT, SYSARG_2);
		char og[PATH_MAX], ng[PATH_MAX], orel[PATH_MAX], nrel[PATH_MAX];
		if (!oa || read_string(tracee, og, oa, sizeof og) <= 0) return false;
		if (!na || read_string(tracee, ng, na, sizeof ng) <= 0) return false;
		int uo = ukfs_rel_at(tracee, AT_FDCWD, og, orel, sizeof orel);
		int un = ukfs_rel_at(tracee, AT_FDCWD, ng, nrel, sizeof nrel);
		if (!uo && !un) return false;
		long r = (uo != un) ? -EXDEV : ukfs_two_path("RENAME", orel, nrel);
		pend_res_set(tracee->pid, nr, r);
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) r); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_chmod) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_1); char gp[PATH_MAX], rel[PATH_MAX];
		if (!pa || read_string(tracee, gp, pa, sizeof gp) <= 0) return false;
		if (!ukfs_rel_at(tracee, AT_FDCWD, gp, rel, sizeof rel)) return false;
		unsigned mode = (unsigned) peek_reg(tracee, CURRENT, SYSARG_2) & 07777;
		char pfx[48]; snprintf(pfx, sizeof pfx, "CHMOD %u ", mode);
		long r = ukfs_simple(pfx, rel); pend_res_set(tracee->pid, nr, r);
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) r); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_chown || nr == PR_lchown) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_1); char gp[PATH_MAX], rel[PATH_MAX];
		if (!pa || read_string(tracee, gp, pa, sizeof gp) <= 0) return false;
		if (!ukfs_rel_at(tracee, AT_FDCWD, gp, rel, sizeof rel)) return false;
		unsigned uid = (unsigned) peek_reg(tracee, CURRENT, SYSARG_2), gid = (unsigned) peek_reg(tracee, CURRENT, SYSARG_3);
		char pfx[64]; snprintf(pfx, sizeof pfx, "CHOWN %u %u ", uid, gid);
		long r = ukfs_simple(pfx, rel); pend_res_set(tracee->pid, nr, r);
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) r); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_stat || nr == PR_lstat || nr == PR_stat64 || nr == PR_lstat64) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_1); char gp[PATH_MAX], rel[PATH_MAX];
		if (!pa || read_string(tracee, gp, pa, sizeof gp) <= 0) return false;
		if (!ukfs_rel_at(tracee, AT_FDCWD, gp, rel, sizeof rel)) return false;
		unsigned mode, uid, gid, nlink; long size, mt, at; unsigned long ino, rdev, blocks;
		int q = ukfs_query_stat(rel, &mode, &uid, &gid, &size, &ino, &mt, &at, &nlink, &rdev, &blocks);
		if (q == -1) return false;
		if (q == -2) { poke_reg(tracee, SYSARG_RESULT, (word_t)(long) - ENOENT); set_sysnum(tracee, PR_void); return true; }
		ukfs_put_stat(tracee, peek_reg(tracee, CURRENT, SYSARG_2), mode, uid, gid, size, ino, mt, at, nlink, rdev, blocks);
		pend_res_set(tracee->pid, nr, 0);
		poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_readlink) {
		word_t pa = peek_reg(tracee, CURRENT, SYSARG_1); char gp[PATH_MAX], rel[PATH_MAX];
		if (!pa || read_string(tracee, gp, pa, sizeof gp) <= 0) return false;
		if (!ukfs_rel_at(tracee, AT_FDCWD, gp, rel, sizeof rel)) return false;  /* legacy path arg: CWD-relative */
		char tgt[PATH_MAX]; long n = ukfs_readlink_at(rel, tgt, sizeof tgt);
		if (n < 0) {
			unsigned m2, u2, g2, nl2; long sz2, mt2, at2; unsigned long i2, rd2, bl2;
			int q2 = ukfs_query_stat(rel, &m2, &u2, &g2, &sz2, &i2, &mt2, &at2, &nl2, &rd2, &bl2);
			long err = (q2 == -2) ? -ENOENT : -EINVAL;
			pend_res_set(tracee->pid, nr, err); poke_reg(tracee, SYSARG_RESULT, (word_t)(long) err); set_sysnum(tracee, PR_void); return true;
		}
		size_t bufsz = (size_t) peek_reg(tracee, CURRENT, SYSARG_3); word_t buf = peek_reg(tracee, CURRENT, SYSARG_2);
		if ((size_t) n > bufsz) n = (long) bufsz;
		if (n > 0) write_data(tracee, buf, tgt, (size_t) n);
		pend_res_set(tracee->pid, nr, n);
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) n); set_sysnum(tracee, PR_void); return true;
	}
	if (nr == PR_symlink) {
		word_t ta = peek_reg(tracee, CURRENT, SYSARG_1), la = peek_reg(tracee, CURRENT, SYSARG_2);
		char target[PATH_MAX], lp[PATH_MAX], rel[PATH_MAX];
		if (!ta || read_string(tracee, target, ta, sizeof target) <= 0) return false;
		if (!la || read_string(tracee, lp, la, sizeof lp) <= 0) return false;
		if (!ukfs_rel_at(tracee, AT_FDCWD, lp, rel, sizeof rel)) return false;
		long r = ukfs_two_path("SYMLINK", target, rel); pend_res_set(tracee->pid, nr, r);
		poke_reg(tracee, SYSARG_RESULT, (word_t)(long) r); set_sysnum(tracee, PR_void); return true;
	}

		return false;
}

/* TEMP DIAG: called at the VERY END of translate_syscall_exit (after fake_id0's
 * SYSCALL_EXIT_END, which can still overwrite the result register). The earlier
 * uknl_fs_open_exit probe runs before that point, so it can't see a late EPERM
 * poke. Log the FINAL result git actually receives for rename/close/open under
 * the vmount — rename always, others only on error — to settle whether the
 * config commit's rename/close is delivered as 0 or mangled to -EPERM. */
/* Authoritatively deliver the result of a PR_void'd write-path syscall. proot's
 * own PR_void restoration clobbers the poked value before the tracee resumes on
 * aarch64 (observed: rename poke=0 -> git receives -1), so re-poke the intended
 * result here, at the very end of translate_syscall_exit. Runs after fake_id0's
 * SYSCALL_EXIT_END, so nothing overwrites it afterwards. */
void uknl_fs_exit_final(Tracee *tracee, word_t nr);
void uknl_fs_exit_final(Tracee *tracee, word_t nr)
{
	if (!uk_fs_on()) return;
	if (g_nm == 0) return;
	long want;
	if (pend_res_take(tracee->pid, nr, &want)) {
		int cur = (int) peek_reg(tracee, CURRENT, SYSARG_RESULT);
		if (cur != (int) want) {
			poke_reg(tracee, SYSARG_RESULT, (word_t)(long) want);
			char l[96]; snprintf(l, sizeof l, "uk_fs: REPOKE nr=%lu %d->%ld\n", (unsigned long) nr, cur, want);
			uk_dbgv_line(l);
		}
	}
}

/* Called from translate_syscall_exit (exit.c) for every syscall exit: binds the
 * fd returned by a redirected openat to its ukfs path in the vfd table. */
void uknl_fs_open_exit(Tracee *tracee, word_t nr)
{
	if (!uk_fs_on()) return;
	/* /dev/fuse: the substituted socket() has returned — chain connect() and record
	 * the channel (phase 2). Keyed per-thread; handles its own nr. */
	uknl_fuse_open_exit(tracee);
	int pid = ukfs_tgid(tracee->pid);   /* vfd table is keyed by thread group (shared fds) */
	int tid = tracee->pid;              /* open_pending is per-thread (same thread enter+exit) */

	/* Track a freshly-opened /dev/loopN[pM] node so raw reads/writes on it serve
	 * from the backing image (blkid/file/fdisk/dd and bare `mount` auto-detect). */
	if (g_lo_any && (nr == PR_openat || nr == PR_openat2 || nr == PR_creat)) {
		long fd = (long)(int) peek_reg(tracee, CURRENT, SYSARG_RESULT);
		if (fd >= 0) {
			char lk[64], pp[PATH_MAX]; snprintf(lk, sizeof lk, "/proc/%d/fd/%ld", tracee->pid, fd);
			ssize_t pn = readlink(lk, pp, sizeof pp - 1);
			if (pn > 0) { pp[pn] = '\0'; uklofd_record(tracee, (int) fd, pp); }
		}
	}

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
			v->isdir = src->isdir; v->off = src->off; v->append = src->append; v->mnt = src->mnt;
			snprintf(v->path, sizeof v->path, "%s", src->path);
		}
		return;
	}

	/* fcntl(fd, F_DUPFD|F_DUPFD_CLOEXEC) is ANOTHER way to dup an fd — glibc's
	 * fdopendir() uses it, so `rm -rf`/`find` read a directory through one fd but
	 * stat/open its children through the DUP. Untracked, those child ops miss the
	 * vfd and leak to the host placeholder (rm sees an "empty" dir and can't
	 * recurse). Alias the new fd to the source vfd, same as dup3. */
	if (nr == PR_fcntl || nr == PR_fcntl64) {
		int cmd = (int) peek_reg(tracee, ORIGINAL, SYSARG_2);
		if (cmd != 0 /*F_DUPFD*/ && cmd != 1030 /*F_DUPFD_CLOEXEC*/) return;
		long newfd = (long)(int) peek_reg(tracee, CURRENT, SYSARG_RESULT);
		if (newfd < 0) return;
		int oldfd = (int) peek_reg(tracee, ORIGINAL, SYSARG_1);
		if ((int) newfd == oldfd) return;
		struct ukfs_vfd *old = vfd_find(pid, (int) newfd);
		if (old) vfd_free(old);
		struct ukfs_vfd *src = vfd_find(pid, oldfd);
		if (!src) return;
		struct ukfs_vfd *v = vfd_alloc();
		if (v) {
			memset(v, 0, sizeof *v);
			v->used = 1; v->pid = pid; v->fd = (int) newfd;
			v->isdir = src->isdir; v->off = src->off; v->append = src->append; v->mnt = src->mnt;
			snprintf(v->path, sizeof v->path, "%s", src->path);
		}
		return;
	}

	if (nr != PR_openat && nr != PR_openat2 && nr != PR_creat) return;
	struct ukfs_pending *pp = NULL;
	for (int i = 0; i < 64; i++)
		if (g_open_pending[i].used && g_open_pending[i].pid == tid) { pp = &g_open_pending[i]; break; }
	if (!pp) return;
	long fd = (long)(int) peek_reg(tracee, CURRENT, SYSARG_RESULT);
	if (fd >= 0) {
		struct ukfs_vfd *v = vfd_find(pid, (int) fd);   /* recycle a stale fd number */
		if (v) vfd_free(v);
		v = vfd_alloc();
		if (v) {
			memset(v, 0, sizeof *v);
			v->used = 1; v->pid = pid; v->fd = (int) fd; v->isdir = pp->isdir; v->off = 0;
			v->append = pp->append; v->mnt = pp->mnt;
			snprintf(v->path, sizeof v->path, "%s", pp->path);
			snprintf(v->backing, sizeof v->backing, "%s", pp->backing);
		}
		/* Unlink the per-fd placeholder now: the guest fd keeps the inode alive
		 * (anonymous), mmap-populate reaches it via /proc/<pid>/fd/<fd>, and it is
		 * auto-freed on close — so no /.ukfs_ph_* files accumulate in the rootfs. */
		if (pp->backing[0]) {
			char host[PATH_MAX];
			if (translate_path(tracee, host, AT_FDCWD, pp->backing, false) >= 0)
				(void) unlink(host);
		}
	}
	pp->used = 0;
}
