/* uKernel — ukfsd: io.neoterm.fs unix-socket server front-end.
 *
 * Dispatches the io.neoterm.fs wire protocol (see docs/USB_STORAGE_MOUNT.md) to
 * the vendored vfs.c ukfs_* ops. vfs.c is path-based and stateless per call, so
 * ukfsd holds no per-fd state: proot owns the vfd table, ukfsd owns the FS.
 *
 * Wire protocol: abstract socket "\0io.neoterm.fs", SOCK_STREAM, one mounted FS
 * per connection. Each request is a '\n'-terminated text line whose PATH field
 * runs to end-of-line (spaces in names OK, newlines rejected). Binary payloads
 * (file data, two-path ops) are framed by explicit byte counts. Replies begin
 * "OK" or "ERR <errno>".
 *
 * Block backend: MOUNT's <devtoken> maps to a block-device path (absolute token
 * used as-is; bare token -> /dev/<token>, override base via UKFSD_DEVDIR). On
 * Android the device is /dev/uksd0, fed by the io.neoterm.block server; the FS
 * engine's existing fd-based block path (vfs.c uk_bio_io) reads it directly. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <sys/prctl.h>

#include "uk_fs_api.h"

#ifndef UKFSD_SOCKET_NAME
#define UKFSD_SOCKET_NAME "io.neoterm.fs"
#endif

/* ---- full-read / full-write over a stream socket ---- */
static int read_n(int fd, void *buf, size_t n)
{
	size_t got = 0;
	while (got < n) {
		ssize_t r = read(fd, (char *)buf + got, n - got);
		if (r == 0) return -1;            /* peer closed mid-frame */
		if (r < 0) { if (errno == EINTR) continue; return -1; }
		got += (size_t)r;
	}
	return 0;
}
static int write_n(int fd, const void *buf, size_t n)
{
	size_t put = 0;
	while (put < n) {
		ssize_t w = write(fd, (const char *)buf + put, n - put);
		if (w < 0) { if (errno == EINTR) continue; return -1; }
		put += (size_t)w;
	}
	return 0;
}

/* Read one '\n'-terminated command line (byte-at-a-time so any following binary
 * payload stays in the socket for read_n). Returns line length (NUL-terminated,
 * newline stripped), -1 on EOF/error, -2 if the line overflows the buffer. */
static int read_line(int fd, char *buf, size_t cap)
{
	size_t i = 0;
	for (;;) {
		char c;
		ssize_t r = read(fd, &c, 1);
		if (r == 0) return -1;
		if (r < 0) { if (errno == EINTR) continue; return -1; }
		if (c == '\n') { buf[i] = '\0'; return (int)i; }
		if (i + 1 >= cap) return -2;
		buf[i++] = c;
	}
}

/* ---- replies ---- */
static int reply(int fd, const char *s)
{
	return write_n(fd, s, strlen(s));
}
static int reply_ok(int fd) { return reply(fd, "OK\n"); }
static int reply_err(int fd, int e)
{
	char b[32];
	if (e < 0) e = -e;
	if (e == 0) e = EIO;
	snprintf(b, sizeof b, "ERR %d\n", e);
	return reply(fd, b);
}
/* ukfs_* return 0/>=0 on success or a negative kernel errno; some legacy ones
 * return a bare -1. Normalize to a positive errno for the wire. */
static int as_errno(long ret, int dflt)
{
	if (ret == -1) return dflt;
	return ret < 0 ? (int)-ret : dflt;
}

/* Normalize an incoming wire path to engine-relative form: the ukfs_* ops are
 * rooted at the mount point and treat "" as root, so strip leading slashes
 * ("/" -> "" -> root, "/sub/f" -> "sub/f"). */
static const char *np(const char *p) { while (*p == '/') p++; return p; }

/* devtoken -> block device path */
static void dev_path(const char *token, char *out, size_t cap)
{
	/* absolute path or explicit "@socket": pass through. */
	if (token[0] == '/' || token[0] == '@') { snprintf(out, cap, "%s", token); return; }
	/* UKFSD_DEVDIR set: file-image backend (used by the host test harness). */
	const char *dir = getenv("UKFSD_DEVDIR");
	if (dir) { snprintf(out, cap, "%s%s", dir, token); return; }
	/* default (Android): the USB block device is served over io.neoterm.block. */
	const char *sock = getenv("UKFSD_BLOCKSOCK");
	snprintf(out, cap, "@%s", sock ? sock : "io.neoterm.block");
}

/* ---- LIST blob: count × {u8 type, u64 ino, u64 size, u16 namelen, name[]} LE ---- */
static void put_u16(char *p, uint16_t v) { p[0]=v; p[1]=v>>8; }
static void put_u64(char *p, uint64_t v) { for (int i=0;i<8;i++) p[i]=v>>(8*i); }

static int do_list(int fd, const char *path)
{
	struct ukfs_dirent *ents = NULL;
	int n = ukfs_list_dir(path, &ents);
	if (n < 0) return reply_err(fd, ENOENT);

	/* size the blob */
	size_t bytes = 0;
	for (int i = 0; i < n; i++) {
		size_t nl = strnlen(ents[i].name, sizeof ents[i].name);
		bytes += 1 + 8 + 8 + 2 + nl;
	}
	char *blob = malloc(bytes ? bytes : 1), *p = blob;
	if (!blob) return reply_err(fd, ENOMEM);
	for (int i = 0; i < n; i++) {
		size_t nl = strnlen(ents[i].name, sizeof ents[i].name);
		*p++ = (char)(uint8_t)ents[i].type;
		put_u64(p, (uint64_t)ents[i].ino);  p += 8;
		put_u64(p, (uint64_t)ents[i].size); p += 8;
		put_u16(p, (uint16_t)nl);           p += 2;
		memcpy(p, ents[i].name, nl);        p += nl;
	}
	char hdr[64];
	snprintf(hdr, sizeof hdr, "OK %d %zu\n", n, bytes);
	int rc = reply(fd, hdr);
	if (rc == 0 && bytes) rc = write_n(fd, blob, bytes);
	free(blob);
	return rc;
}

static int do_read(int fd, long long off, size_t len, const char *path)
{
	char *buf = malloc(len ? len : 1);
	if (!buf) return reply_err(fd, ENOMEM);
	long n = ukfs_read_file(path, buf, len, (long)off);
	if (n < 0) { free(buf); return reply_err(fd, ENOENT); }
	char hdr[48];
	snprintf(hdr, sizeof hdr, "OK %ld\n", n);
	int rc = reply(fd, hdr);
	if (rc == 0 && n) rc = write_n(fd, buf, (size_t)n);
	free(buf);
	return rc;
}

static int do_write(int fd, long long off, size_t len, const char *path)
{
	char *buf = malloc(len ? len : 1);
	if (!buf) return reply_err(fd, ENOMEM);
	if (read_n(fd, buf, len) < 0) { free(buf); return -1; }   /* frame broken */
	long n = ukfs_write_file_at(path, buf, len, off);
	free(buf);
	if (n < 0) return reply_err(fd, as_errno(n, EIO));
	char hdr[48];
	snprintf(hdr, sizeof hdr, "OK %ld\n", (long)len);
	return reply(fd, hdr);
}

static int do_readlink(int fd, const char *path)
{
	char buf[4096];
	long n = ukfs_readlink(path, buf, sizeof buf);
	if (n < 0) return reply_err(fd, as_errno(n, EINVAL));
	char hdr[48];
	snprintf(hdr, sizeof hdr, "OK %ld\n", n);
	int rc = reply(fd, hdr);
	if (rc == 0 && n) rc = write_n(fd, buf, (size_t)n);
	return rc;
}

static int do_stat(int fd, const char *path)
{
	unsigned int mode=0, uid=0, gid=0, nlink=0;
	long size=0, mtime=0, atime=0; unsigned long ino=0, rdev=0, blocks=0;
	if (ukfs_stat(path, &mode, &uid, &gid, &size, &ino, &mtime, &atime,
	              &nlink, &rdev, &blocks) != 0)
		return reply_err(fd, ENOENT);
	char b[192];
	snprintf(b, sizeof b, "OK %u %u %u %ld %lu %ld %ld %u %lu %lu\n",
	         mode, uid, gid, size, ino, mtime, atime, nlink, rdev, blocks);
	return reply(fd, b);
}

static int do_statfs(int fd)
{
	unsigned long bsize=0; unsigned long long blocks=0, bfree=0, bavail=0, files=0, ffree=0;
	long namelen=0, frsize=0, ftype=0;
	if (ukfs_statfs(&bsize, &blocks, &bfree, &bavail, &files, &ffree,
	                &namelen, &frsize, &ftype) != 0)
		return reply_err(fd, EIO);
	char b[224];
	snprintf(b, sizeof b, "OK %lu %llu %llu %llu %llu %llu %ld %ld %ld\n",
	         bsize, blocks, bfree, bavail, files, ffree, namelen, frsize, ftype);
	return reply(fd, b);
}

/* RENAME/SYMLINK: two byte-counted paths concatenated in the payload. */
static int do_two_path(int fd, const char *cmd, size_t alen, size_t blen)
{
	char *a = malloc(alen + 1), *b = malloc(blen + 1);
	if (!a || !b) { free(a); free(b); return reply_err(fd, ENOMEM); }
	if (read_n(fd, a, alen) < 0 || read_n(fd, b, blen) < 0) {
		free(a); free(b); return -1;
	}
	a[alen] = b[blen] = '\0';
	/* RENAME: both are FS paths. SYMLINK: target (a) is stored verbatim, only
	 * the link path (b) is an FS path. */
	long r = (cmd[0]=='R') ? ukfs_rename(np(a), np(b)) : ukfs_symlink(a, np(b));
	free(a); free(b);
	return r < 0 ? reply_err(fd, as_errno(r, EIO)) : reply_ok(fd);
}

/* Dispatch one request line (payloads read inline). Returns 0 to keep the
 * connection, -1 to drop it (EOF or a broken binary frame). */
static int handle(int fd, char *line)
{
	long long n1, n2, n3, n4; size_t l1, l2; int pos = 0;
	char fstype[64], token[512];

	if (sscanf(line, "MOUNT %63s %511s", fstype, token) == 2) {
		char path[600]; dev_path(token, path, sizeof path);
		fprintf(stderr, "ukfsd: MOUNT recv fstype=%s token=%s -> path=%s\n", fstype, token, path); fflush(stderr);
		if (strcmp(fstype, "auto") == 0) {
			/* probe the engine's filesystems in turn (matches libukfs_all's
			 * lazy mount). uk_mount rejects an unregistered name before it
			 * touches the device, so a miss is cheap. */
			static const char *cands[] = { "vfat", "exfat", "ntfs3", "ext4", NULL };
			for (int i = 0; cands[i]; i++) {
				fprintf(stderr, "ukfsd: MOUNT auto trying %s on %s\n", cands[i], path); fflush(stderr);
				if (ukfs_mount(cands[i], path) == 0) {
					fprintf(stderr, "ukfsd: MOUNT %s OK\n", cands[i]); fflush(stderr);
					return reply_ok(fd);
				}
			}
			fprintf(stderr, "ukfsd: MOUNT auto: all candidates failed\n"); fflush(stderr);
			return reply_err(fd, ENODEV);
		}
		return ukfs_mount(fstype, path) == 0 ? reply_ok(fd) : reply_err(fd, ENODEV);
	}
	if (!strcmp(line, "UMOUNT"))  return reply_ok(fd);
	if (!strcmp(line, "STATFS"))  return do_statfs(fd);

	if (!strncmp(line, "STAT ", 5))     return do_stat(fd, np(line + 5));
	if (!strncmp(line, "LIST ", 5))     return do_list(fd, np(line + 5));
	if (!strncmp(line, "READLINK ", 9)) return do_readlink(fd, np(line + 9));
	if (!strncmp(line, "UNLINK ", 7))   { long r = ukfs_unlink(np(line+7)); return r<0?reply_err(fd,as_errno(r,ENOENT)):reply_ok(fd); }
	if (!strncmp(line, "RMDIR ", 6))    { long r = ukfs_rmdir(np(line+6)); return r<0?reply_err(fd,as_errno(r,ENOTEMPTY)):reply_ok(fd); }

	if (sscanf(line, "READ %lld %zu %n", &n1, &l1, &pos) == 2 && pos)
		return do_read(fd, n1, l1, np(line + pos));
	if (sscanf(line, "WRITE %lld %zu %n", &n1, &l1, &pos) == 2 && pos)
		return do_write(fd, n1, l1, np(line + pos));
	if (sscanf(line, "CREATE %lld %n", &n1, &pos) == 1 && pos)
		{ long r = ukfs_write_file(np(line+pos), "", 0); return r<0?reply_err(fd,as_errno(r,EIO)):reply_ok(fd); }
	if (sscanf(line, "MKDIR %lld %n", &n1, &pos) == 1 && pos)
		{ const char *p=np(line+pos); long r = ukfs_mkdir(p); if (r==0 && n1) ukfs_chmod(p,(unsigned)n1); return r<0?reply_err(fd,as_errno(r,EIO)):reply_ok(fd); }
	if (sscanf(line, "TRUNCATE %lld %n", &n1, &pos) == 1 && pos)
		{ long r = ukfs_truncate(np(line+pos), n1); return r<0?reply_err(fd,as_errno(r,EIO)):reply_ok(fd); }
	if (sscanf(line, "CHMOD %lld %n", &n1, &pos) == 1 && pos)
		{ long r = ukfs_chmod(np(line+pos),(unsigned)n1); return r<0?reply_err(fd,as_errno(r,EIO)):reply_ok(fd); }
	if (sscanf(line, "CHOWN %lld %lld %n", &n1, &n2, &pos) == 2 && pos)
		{ long r = ukfs_chown(np(line+pos),(unsigned)n1,(unsigned)n2); return r<0?reply_err(fd,as_errno(r,EIO)):reply_ok(fd); }
	if (sscanf(line, "UTIME %lld %lld %lld %lld %n", &n1,&n2,&n3,&n4,&pos) == 4 && pos)
		{ long r = ukfs_utime(np(line+pos), n1,n2,n3,n4); return r<0?reply_err(fd,as_errno(r,EIO)):reply_ok(fd); }
	if (sscanf(line, "RENAME %zu %zu", &l1, &l2) == 2)  return do_two_path(fd, "R", l1, l2);
	if (sscanf(line, "SYMLINK %zu %zu", &l1, &l2) == 2) return do_two_path(fd, "S", l1, l2);

	return reply(fd, "ERR 22\n");   /* EINVAL: unknown command */
}

static void serve(int cfd)
{
	char line[8192];
	for (;;) {
		int r = read_line(cfd, line, sizeof line);
		if (r == -1) return;                    /* client closed */
		if (r == -2) { reply(cfd, "ERR 36\n"); continue; }  /* ENAMETOOLONG */
		if (handle(cfd, line) < 0) return;      /* broken frame -> drop */
	}
}

int main(int argc, char **argv)
{
	const char *sockname = (argc > 1) ? argv[1] : UKFSD_SOCKET_NAME;
	/* Die when the launching app process dies, so a stale ukfsd can't keep
	 * holding @io.neoterm.fs across app restarts (which would shadow the new
	 * build). PR_SET_PDEATHSIG fires when our parent (the app) exits. */
	prctl(PR_SET_PDEATHSIG, SIGKILL);
	setenv("UK_FS_DEBUG", "1", 0);   /* TEMP: verbose engine mount logging -> ukfsd.log */
	setenv("UK_LOGLEVEL", "7", 0);
	signal(SIGPIPE, SIG_IGN);

	int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sfd < 0) { perror("socket"); return 1; }

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	/* abstract namespace: leading NUL, name follows */
	size_t nl = strlen(sockname);
	if (nl + 1 > sizeof addr.sun_path) { fprintf(stderr, "socket name too long\n"); return 1; }
	addr.sun_path[0] = '\0';
	memcpy(addr.sun_path + 1, sockname, nl);
	socklen_t alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + nl);

	if (bind(sfd, (struct sockaddr *)&addr, alen) < 0) { perror("bind"); return 1; }
	if (listen(sfd, 4) < 0) { perror("listen"); return 1; }
	fprintf(stderr, "ukfsd: listening on @%s\n", sockname);

	for (;;) {
		int cfd = accept(sfd, NULL, NULL);
		if (cfd < 0) { if (errno == EINTR) continue; perror("accept"); break; }
		serve(cfd);
		close(cfd);
		/* one FS per connection: drop it so the next client mounts fresh */
	}
	close(sfd);
	return 0;
}
