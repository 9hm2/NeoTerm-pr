#!/usr/bin/env python3
# Patch the Termux proot fork's fake_id0 extension to store emulated ownership
# (uid/gid/mode) in a `user.proot.meta` extended attribute on each file, instead
# of `.proot-meta-file.*` sidecar files (USERLAND mode's default) — no rootfs
# clutter. This makes chown persistent and CALLER-INDEPENDENT, so the
# "create as root -> chown user -> start via a root wrapper" pattern (Debian's
# pg_createcluster / pg_ctlcluster, and similar) works under -0 fake root.
#
# Must be paired with -DUSERLAND in the proot build (see build-proot.sh). The
# storage functions are verified on a host with fakeid0-xattr-test/run.sh.
#
# Usage: fakeid0-xattr.py <proot-src-dir>   (the dir containing extension/)

import sys, re
ROOT = sys.argv[1]
def rd(p): return open(p, encoding='utf-8', errors='surrogateescape').read()
def wr(p, s): open(p,'w',encoding='utf-8',errors='surrogateescape').write(s)
def must(c,m):
    if not c: sys.stderr.write("PATCH FAIL: %s\n"%m); sys.exit(9)
FK = ROOT + "/extension/fake_id0/"

# ---- helper_functions.c : xattr storage ----
hf = FK+"helper_functions.c"; s = rd(hf)
must('XATTR_META_NAME' not in s, "helper already patched")
s = s.replace('#define META_TAG ".proot-meta-file."',
  '#include <sys/xattr.h>\n#define META_TAG ".proot-meta-file."\n#define XATTR_META_NAME "user.proot.meta"', 1)
must('XATTR_META_NAME' in s, "insert include/define")

get_meta_new = r'''int get_meta_path(char orig_path[PATH_MAX], char meta_path[PATH_MAX])
{
	/* xattr-backed: the meta lives in an xattr on the file itself, so the
	 * "meta path" is just the real path (no .proot-meta-file sidecars). */
	strncpy(meta_path, orig_path, PATH_MAX - 1);
	meta_path[PATH_MAX - 1] = '\0';
	return 0;
}
'''
s2 = re.sub(r'int get_meta_path\(char orig_path\[PATH_MAX\], char meta_path\[PATH_MAX\]\).*?\n\}\n',
            lambda _m: get_meta_new, s, count=1, flags=re.S)
must(s2!=s, "replace get_meta_path"); s=s2

read_meta_new = r'''int read_meta_file(char path[PATH_MAX], mode_t *mode, uid_t *owner, gid_t *group, Config *config)
{
	char buf[64];
	int lcl_mode;
	ssize_t n = getxattr(path, XATTR_META_NAME, buf, sizeof(buf) - 1);
	if(n <= 0) {
		/* No meta xattr: permissive default (as upstream's missing-meta case). */
		*owner = config->euid;
		*group = config->egid;
		*mode = otod(755);
		return 0;
	}
	buf[n] = '\0';
	if(sscanf(buf, "%d %d %d", &lcl_mode, owner, group) != 3) {
		*owner = config->euid;
		*group = config->egid;
		*mode = otod(755);
		return 0;
	}
	*mode = (mode_t) otod(lcl_mode);
	return 0;
}
'''
s2 = re.sub(r'int read_meta_file\(char path\[PATH_MAX\].*?\n\}\n', lambda _m: read_meta_new, s, count=1, flags=re.S)
must(s2!=s, "replace read_meta_file"); s=s2

write_meta_new = r'''int write_meta_file(char path[PATH_MAX], mode_t mode, uid_t owner, gid_t group,
	bool is_creat, Config *config)
{
	char buf[64];
	int len;
	if(is_creat)
		mode = (mode & ~(config->umask) & 0777);
	len = snprintf(buf, sizeof(buf), "%d\n%d\n%d\n", dtoo(mode), owner, group);
	if(len <= 0 || (size_t) len >= sizeof(buf))
		return -1;
	/* Non-fatal: write_meta is also called at syscall ENTER for mkdir/creat,
	 * when the target does not exist yet (setxattr -> ENOENT), and the fs/SELinux
	 * may reject it. The guest syscall must still succeed; chown persists the
	 * owner later, when the file exists. */
	setxattr(path, XATTR_META_NAME, buf, len, 0);
	return 0;
}

/* xattr-backed meta existence (mirrors path_exists semantics: 0 = present). */
int meta_exists(char path[PATH_MAX])
{
	return (getxattr(path, XATTR_META_NAME, NULL, 0) >= 0) ? 0 : -1;
}
'''
s2 = re.sub(r'int write_meta_file\(char path\[PATH_MAX\].*?\n\}\n', lambda _m: write_meta_new, s, count=1, flags=re.S)
must(s2!=s, "replace write_meta_file"); s=s2
wr(hf, s)

# ---- helper_functions.h : meta_exists prototype ----
hh = FK+"helper_functions.h"; s = rd(hh)
must('int path_exists(char path[PATH_MAX]);' in s, "path_exists proto present")
s = s.replace('int path_exists(char path[PATH_MAX]);',
              'int path_exists(char path[PATH_MAX]);\nint meta_exists(char path[PATH_MAX]);', 1)
wr(hh, s)

# ---- chown.c: drop the "no meta -> skip" early-return so chown ALWAYS persists
#      (by chown time the file exists, so the xattr write in write_meta succeeds) ----
cf=FK+"chown.c"; s=rd(cf)
old_cf="\tif(path_exists(meta_path) != 0)\n\t\treturn 0;\n"
must(old_cf in s, "chown.c early-return block")
s=s.replace(old_cf, "\t/* xattr-backed: always persist ownership on chown. */\n", 1)
wr(cf, s)
# ---- open.c / stat.c: path_exists(meta_path) -> meta_exists(meta_path) ----
for f in ["open.c","stat.c"]:
    p=FK+f; s=rd(p)
    must('path_exists(meta_path)' in s, "%s path_exists(meta_path)"%f)
    s=s.replace('path_exists(meta_path)','meta_exists(meta_path)')
    wr(p,s)

# ---- config.h: per-tracee deferred create-meta state ----
ch=FK+"config.h"; s=rd(ch)
must('meta_pending' not in s, "config.h already patched")
s=s.replace('#include <sys/types.h>   /* uid_t, gid_t */',
            '#include <sys/types.h>   /* uid_t, gid_t */\n#include <linux/limits.h> /* PATH_MAX */',1)
s=s.replace('\tbool keep_caps;\n} Config;',
            '\tbool keep_caps;\n\n'
            '\t/* xattr-backed create-meta: handle_open/handle_mk record a new file\'s\n'
            '\t * intended mode here at syscall ENTER (the file does not exist yet, so the\n'
            '\t * xattr cannot be set then); handle_sysexit_end writes it once the create\n'
            '\t * syscall has succeeded and the file exists. */\n'
            '\tbool meta_pending;\n\tmode_t meta_pending_mode;\n\tchar meta_pending_path[PATH_MAX];\n} Config;',1)
must('meta_pending_path' in s, "config.h fields"); wr(ch,s)

# ---- open.c: new-file create defers the meta write to EXIT ----
of=FK+"open.c"; s=rd(of)
old_o=("\t\tmode = peek_reg(tracee, ORIGINAL, mode_sysarg);\n"
       "\t\tpoke_reg(tracee, mode_sysarg, (mode|0700));\n"
       "\t\tstatus = write_meta_file(meta_path, mode, config->euid, config->egid, 1, config);\n"
       "\t\treturn status;")
new_o=("\t\tmode = peek_reg(tracee, ORIGINAL, mode_sysarg);\n"
       "\t\tpoke_reg(tracee, mode_sysarg, (mode|0700));\n"
       "\t\t/* xattr-backed: the file does not exist yet; defer the meta write to EXIT. */\n"
       "\t\tconfig->meta_pending = true;\n"
       "\t\tconfig->meta_pending_mode = mode;\n"
       "\t\tstrncpy(config->meta_pending_path, meta_path, PATH_MAX - 1);\n"
       "\t\tconfig->meta_pending_path[PATH_MAX - 1] = 0;\n"
       "\t\treturn 0;")
must(old_o in s, "open.c new-file block"); s=s.replace(old_o,new_o,1); wr(of,s)

# ---- open.c: the /dev/kmsg writable buffer (NeoTerm binds a regular file over
#      /dev/kmsg so dmesg works) must behave like the kernel ring buffer: every
#      write APPENDS and O_TRUNC is ignored, otherwise `echo x > /dev/kmsg` (the
#      common idiom) would truncate the buffer and dmesg would only ever show the
#      last message. Force O_APPEND + drop O_TRUNC when opening that path. No-op
#      for every other path, so it cannot affect unrelated files. ----
ok=FK+"open.c"; s=rd(ok)
if '#include <string.h>' not in s:
    s=s.replace('#include <fcntl.h>', '#include <fcntl.h>\n#include <string.h>', 1)
must('#include <string.h>' in s, "open.c string.h include")
kanchor='\t\tflags = O_CREAT;'
must(kanchor in s, "open.c flags=O_CREAT anchor")
kinject=('\t\tflags = O_CREAT;\n'
         '\t/* NeoTerm: /dev/kmsg backing file -> kernel-ring-buffer semantics. */\n'
         '\tif (flags_sysarg != IGNORE_SYSARG) {\n'
         '\t\tsize_t _kl = strlen(orig_path);\n'
         '\t\tif ((_kl >= 9  && strcmp(orig_path + _kl - 9,  "/dev/kmsg") == 0) ||\n'
         '\t\t    (_kl >= 13 && strcmp(orig_path + _kl - 13, "/sysdata/kmsg") == 0)) {\n'
         '\t\t\tflags = (flags & ~O_TRUNC) | O_APPEND;\n'
         '\t\t\tpoke_reg(tracee, flags_sysarg, flags);\n'
         '\t\t}\n'
         '\t}')
s=s.replace(kanchor, kinject, 1); wr(ok, s)

# ---- mk.c: mkdir/mknod defers the meta write to EXIT ----
mf=FK+"mk.c"; s=rd(mf)
old_m=("\tmode = peek_reg(tracee, ORIGINAL, mode_sysarg);\n"
       "\tpoke_reg(tracee, mode_sysarg, (mode|0700));\n"
       "\treturn write_meta_file(meta_path, mode, config->euid, config->egid, 1, config);")
new_m=("\tmode = peek_reg(tracee, ORIGINAL, mode_sysarg);\n"
       "\tpoke_reg(tracee, mode_sysarg, (mode|0700));\n"
       "\t/* xattr-backed: defer the meta write to EXIT (dir/node does not exist yet). */\n"
       "\tconfig->meta_pending = true;\n"
       "\tconfig->meta_pending_mode = mode;\n"
       "\tstrncpy(config->meta_pending_path, meta_path, PATH_MAX - 1);\n"
       "\tconfig->meta_pending_path[PATH_MAX - 1] = 0;\n"
       "\treturn 0;")
must(old_m in s, "mk.c write block"); s=s.replace(old_m,new_m,1); wr(mf,s)

# ---- fake_id0.c: flush the deferred create-meta at the top of handle_sysexit_end ----
xf=FK+"fake_id0.c"; s=rd(xf)
old_x=("\tsysnum = get_sysnum(tracee, ORIGINAL);\n\n#ifdef USERLAND\n"
       "\tif ((get_sysnum(tracee, CURRENT) == PR_fstat) || (get_sysnum(tracee, CURRENT) == PR_fstat64)) {")
new_x=("\tsysnum = get_sysnum(tracee, ORIGINAL);\n\n#ifdef USERLAND\n"
       "\t/* xattr-backed: flush a deferred create-meta (open/creat/mkdir/mknod recorded it\n"
       "\t * at ENTER, when the target did not exist). The syscall has now run; persist the\n"
       "\t * intended mode + creator id only if it succeeded (the file exists). */\n"
       "\tif (config->meta_pending) {\n"
       "\t\tconfig->meta_pending = false;\n"
       "\t\tif ((long) peek_reg(tracee, CURRENT, SYSARG_RESULT) >= 0)\n"
       "\t\t\twrite_meta_file(config->meta_pending_path, config->meta_pending_mode,\n"
       "\t\t\t\tconfig->euid, config->egid, 1, config);\n"
       "\t}\n"
       "\tif ((get_sysnum(tracee, CURRENT) == PR_fstat) || (get_sysnum(tracee, CURRENT) == PR_fstat64)) {")
must(old_x in s, "fake_id0.c sysexit head"); s=s.replace(old_x,new_x,1); wr(xf,s)

# ---- rename.c: the xattr travels with the inode on rename(2); drop the meta
#      "copy", whose unlink(meta_path) now == unlink(the real file) -> ENOENT ----
rf=FK+"rename.c"; s=rd(rf)
rn_new=("\t/* xattr-backed: the user.proot.meta xattr travels with the inode on rename(2),\n"
        "\t * so there is nothing to copy. (The old sidecar logic did unlink(meta_path),\n"
        "\t * which now == the real file -> it would delete the file before the rename.) */\n"
        "\t(void) meta_path; (void) uid; (void) gid; (void) mode;\n"
        "\treturn 0;\n")
rn_pat=r'\t// If a meta file exists.*?return write_meta_file\(meta_path, mode, uid, gid, 0, config\);[ \t]*\n'
s2=re.sub(rn_pat, lambda _m: rn_new, s, count=1, flags=re.S)
must(s2!=s, "rename.c meta block")
wr(rf, s2)

# ---- unlink.c: the meta xattr dies with the inode on real unlink(2); the old
#      unlink(meta_path) now == unlink(the real file) before the syscall ----
uf=FK+"unlink.c"; s=rd(uf)
up=r'\tif\(path_exists\(meta_path\) == 0\)[ \t]*\n\t\tunlink\(meta_path\);\n'
un=("\t/* xattr-backed: the meta xattr is part of the inode and is removed with it\n"
    "\t * by the real unlink(2); unlinking meta_path (== the real file) would delete it. */\n"
    "\t(void) meta_path;\n")
s2=re.sub(up, lambda _m: un, s, count=1, flags=re.S)
must(s2!=s, "unlink.c meta block"); wr(uf, s2)

# ---- fake_id0.c: LINK2SYMLINK_RENAME/_UNLINK move/delete the meta; with xattr
#      that hits the real file (link2symlink already moved/removed the inode) ----
ff=FK+"fake_id0.c"; s=rd(ff)
for name in ["LINK2SYMLINK_RENAME","LINK2SYMLINK_UNLINK"]:
    pat=r'case '+name+r': \{.*?\n\t\}\n'
    rep=("case "+name+":\n"
         "\t\t/* xattr-backed: ownership lives in the inode's xattr, which link2symlink's\n"
         "\t\t * real rename/unlink already carries/removes. Nothing to do. */\n"
         "\t\treturn 0;\n")
    s2=re.sub(pat, lambda _m,_r=rep: _r, s, count=1, flags=re.S)
    must(s2!=s, name); s=s2
wr(ff, s)

# ---- fake_id0.c: USERLAND fstat() resolves the fd via readlinkat(/proc/pid/fd/N).
#      For a socket / pipe / anon_inode / eventfd / timerfd / memfd, that link is
#      NOT a filesystem path ("socket:[12345]", "anon_inode:[eventfd]", "[timerfd]"
#      ...), yet the code then chained newfstatat(AT_FDCWD, <that>, ...) -> ENOENT,
#      so fstat(2) on ANY socket failed (e.g. the Ruby pg / PostgreSQL client:
#      "No such file or directory - fstat(2)"). A real file from /proc/pid/fd
#      always starts with '/'; anything else must fall back to a plain re-issued
#      fstat, exactly like the existing "pipe"/" (deleted)" special-cases. ----
gf=FK+"fake_id0.c"; s=rd(gf)
old_fd='if ((strcmp(path + strlen(path) - strlen(" (deleted)"), " (deleted)") == 0) || (strncmp(path, "pipe", 4) == 0)) {'
new_fd='if ((path[0] != \'/\') || (strcmp(path + strlen(path) - strlen(" (deleted)"), " (deleted)") == 0) || (strncmp(path, "pipe", 4) == 0)) {'
must(old_fd in s, "fake_id0.c fstat readlinkat condition"); s=s.replace(old_fd,new_fd,1); wr(gf,s)

# ---- stat.c : statx handler reads the xattr; add include ----
sp=FK+"stat.c"; s=rd(sp)
if '#include <sys/xattr.h>' not in s:
    s=s.replace('#include "tracee/statx.h"','#include "tracee/statx.h"\n#include <sys/xattr.h>\n#define XATTR_META_NAME "user.proot.meta"',1)
must('XATTR_META_NAME' in s, "stat.c include/define")
statx_new = r'''int fake_id0_handle_statx_syscall(Tracee *tracee, Config *config, uintptr_t statx_state_raw) {
	struct statx_syscall_state *state = (struct statx_syscall_state *) statx_state_raw;
	/* xattr-backed ownership: statx() is what modern glibc stat() uses, so it
	 * must honour the persistent owner stored by chown (user.proot.meta xattr). */
	char path[PATH_MAX];
	if (read_sysarg_path(tracee, path, SYSARG_2, MODIFIED) == 0) {
		char buf[64];
		ssize_t n = getxattr(path, XATTR_META_NAME, buf, sizeof(buf) - 1);
		if (n > 0) {
			int m, o, g;
			buf[n] = '\0';
			if (sscanf(buf, "%d %d %d", &m, &o, &g) == 3) {
				if (state->statx_buf.stx_mask & STATX_MODE) {
					state->statx_buf.stx_mode = (unsigned short)(((mode_t) otod(m))
						| (state->statx_buf.stx_mode & S_IFMT)
						| (state->statx_buf.stx_mode & 07000));
					state->updated_stats = true;
				}
				if (state->statx_buf.stx_mask & STATX_UID) {
					state->statx_buf.stx_uid = (uint32_t) o;
					state->updated_stats = true;
				}
				if (state->statx_buf.stx_mask & STATX_GID) {
					state->statx_buf.stx_gid = (uint32_t) g;
					state->updated_stats = true;
				}
				return 0;
			}
		}
	}
	/* Fallback: report the effective uid/gid for app-owned files. */
	if (state->statx_buf.stx_mask & STATX_UID) {
		if (state->statx_buf.stx_uid == getuid()) {
			state->statx_buf.stx_uid = config->euid;
			state->updated_stats = true;
		}
	}
	if (state->statx_buf.stx_mask & STATX_GID) {
		if (state->statx_buf.stx_gid == getgid()) {
			state->statx_buf.stx_gid = config->egid;
			state->updated_stats = true;
		}
	}
	return 0;
}
'''
s2 = re.sub(r'int fake_id0_handle_statx_syscall\(.*?\n\}\n', lambda _m: statx_new, s, count=1, flags=re.S)
must(s2!=s, "replace statx handler"); s=s2
wr(sp, s)

# ---- syscall/enter.c: (a) redirect open/stat of the virtual hotplug port
#      /dev/ttyUSB<n> to its current live PTY slave (queried from the app), and
#      (b) proxy the PTY modem-control / break ioctls (TIOCMGET/SET/BIS/BIC,
#      TCSBRK, TIOCSBRK/CBRK) to the app over the io.neoterm.ttyusb abstract
#      socket, so DTR/RTS/CTS/DSR/DCD/RI/BREAK reach the real chip. The string
#      guard means non-ttyUSB / non-serial paths are completely unaffected. ----
EN = ROOT + "/syscall/enter.c"; s = rd(EN)
if 'uknl_modem_call' not in s:
    must('#include <sys/ioctl.h>' in s, "enter.c sys/ioctl include")
    s = s.replace('#include <sys/ioctl.h>   /* ioctl(2): SIOCGIFMTU / SIOCGIFHWADDR */',
                  '#include <sys/ioctl.h>   /* ioctl(2): SIOCGIFMTU / SIOCGIFHWADDR */\n'
                  '#include <stdlib.h>      /* atoi(3) — NeoTerm usb-serial proxy */', 1)
    func = (
        '/* NeoTerm: persistent connection to the app-side usb-serial server. The\n'
        ' * modem-line ioctls (DTR/RTS) fire in tight, timing-sensitive bursts during\n'
        ' * esptool/avrdude auto-reset; reconnecting per call added socket setup/teardown\n'
        ' * jitter that made the reset unreliable. Keep one socket open (the proot tracer\n'
        ' * event loop is single-threaded, so these calls are serialized) and lazily\n'
        ' * reconnect only if the cached socket has died. */\n'
        'static int uknl_persist_fd = -1;\n'
        '\n'
        'static int uknl_connect(void)\n'
        '{\n'
        '\tint s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);\n'
        '\tif (s < 0)\n\t\treturn -1;\n'
        '\tstruct timeval tv = { .tv_sec = 1, .tv_usec = 0 };\n'
        '\tsetsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);\n'
        '\tsetsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);\n'
        '\tstruct sockaddr_un a;\n'
        '\tmemset(&a, 0, sizeof a);\n'
        '\ta.sun_family = AF_UNIX;\n'
        '\tconst char *name = "io.neoterm.ttyusb";\n'
        '\ta.sun_path[0] = \'\\0\';\t\t\t\t/* abstract namespace */\n'
        '\tstrncpy(a.sun_path + 1, name, sizeof(a.sun_path) - 2);\n'
        '\tsocklen_t len = sizeof(a.sun_family) + 1 + strlen(name);\n'
        '\tif (connect(s, (struct sockaddr *) &a, len) < 0) {\n'
        '\t\tclose(s);\n'
        '\t\treturn -1;\n'
        '\t}\n'
        '\treturn s;\n'
        '}\n'
        '\n'
        '/* One newline-terminated request/reply on an already-connected socket. */\n'
        'static bool uknl_io(int s, const char *req, char *resp, size_t resp_sz)\n'
        '{\n'
        '\tsize_t rl = strlen(req);\n'
        '\tif (write(s, req, rl) != (ssize_t) rl)\n'
        '\t\treturn false;\n'
        '\tssize_t n = read(s, resp, resp_sz - 1);\n'
        '\tif (n <= 0)\n'
        '\t\treturn false;\n'
        '\tresp[n] = \'\\0\';\n'
        '\twhile (n > 0 && (resp[n - 1] == \'\\n\' || resp[n - 1] == \'\\r\'))\n'
        '\t\tresp[--n] = \'\\0\';\n'
        '\treturn true;\n'
        '}\n'
        '\n'
        '/* NeoTerm: one request/reply to the usb-serial server over the persistent\n'
        ' * connection; reconnect once and retry if the cached socket is stale. */\n'
        'static bool uknl_modem_call(const char *req, char *resp, size_t resp_sz)\n'
        '{\n'
        '\tfor (int attempt = 0; attempt < 2; attempt++) {\n'
        '\t\tif (uknl_persist_fd < 0)\n'
        '\t\t\tuknl_persist_fd = uknl_connect();\n'
        '\t\tif (uknl_persist_fd < 0)\n'
        '\t\t\treturn false;\n'
        '\t\tif (uknl_io(uknl_persist_fd, req, resp, resp_sz))\n'
        '\t\t\treturn true;\n'
        '\t\tclose(uknl_persist_fd);\t\t\t/* stale/broken -> drop, retry fresh */\n'
        '\t\tuknl_persist_fd = -1;\n'
        '\t}\n'
        '\treturn false;\n'
        '}\n'
        '\n'
        '/* NeoTerm: rewrite /dev/ttyUSB<n> to its current live PTY slave. Leaves the\n'
        ' * path unchanged when there is no live port (-> normal ENOENT). */\n'
        'static void uknl_ttyusb_redirect(char path[PATH_MAX])\n'
        '{\n'
        '\tconst char *base = strrchr(path, \'/\');\n'
        '\tif (base == NULL)\n\t\treturn;\n'
        '\tchar req[64], resp[PATH_MAX];\n'
        '\tsnprintf(req, sizeof(req), "PATH %s\\n", base + 1);\n'
        '\tif (uknl_modem_call(req, resp, sizeof(resp)) && strncmp(resp, "/dev/pts/", 9) == 0)\n'
        '\t\tstrncpy(path, resp, PATH_MAX - 1);\n'
        '}\n'
        '\n'
        '/* NeoTerm: rewrite /dev/iio:device<n> to its live PTY (buffered IIO sample\n'
        ' * stream from the sensor bridge). One-shot query to io.neoterm.iio (opens are\n'
        ' * rare); leaves the path unchanged when there is no stream (-> ENOENT, so\n'
        ' * libiio falls back to the sysfs polled channels). */\n'
        'static void uknl_iio_redirect(char path[PATH_MAX])\n'
        '{\n'
        '\tconst char *base = strrchr(path, \'/\');\n'
        '\tif (base == NULL)\n\t\treturn;\n'
        '\tint s = socket(AF_UNIX, SOCK_STREAM, 0);\n'
        '\tif (s < 0)\n\t\treturn;\n'
        '\tstruct timeval tv = { .tv_sec = 1, .tv_usec = 0 };\n'
        '\tsetsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);\n'
        '\tsetsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);\n'
        '\tstruct sockaddr_un a;\n'
        '\tmemset(&a, 0, sizeof a);\n'
        '\ta.sun_family = AF_UNIX;\n'
        '\tconst char *name = "io.neoterm.iio";\n'
        '\ta.sun_path[0] = \'\\0\';\n'
        '\tstrncpy(a.sun_path + 1, name, sizeof(a.sun_path) - 2);\n'
        '\tsocklen_t len = sizeof(a.sun_family) + 1 + strlen(name);\n'
        '\tif (connect(s, (struct sockaddr *) &a, len) < 0) {\n'
        '\t\tclose(s);\n'
        '\t\treturn;\n'
        '\t}\n'
        '\tchar req[64], resp[PATH_MAX];\n'
        '\tsnprintf(req, sizeof(req), "PATH %s\\n", base + 1);\n'
        '\tsize_t rl = strlen(req);\n'
        '\tssize_t n = (write(s, req, rl) == (ssize_t) rl) ? read(s, resp, sizeof(resp) - 1) : -1;\n'
        '\tclose(s);\n'
        '\tif (n <= 0)\n\t\treturn;\n'
        '\tresp[n] = \'\\0\';\n'
        '\twhile (n > 0 && (resp[n - 1] == \'\\n\' || resp[n - 1] == \'\\r\'))\n'
        '\t\tresp[--n] = \'\\0\';\n'
        '\tif (strncmp(resp, "/dev/pts/", 9) == 0)\n'
        '\t\tstrncpy(path, resp, PATH_MAX - 1);\n'
        '}\n'
        '\n'
        '/* NeoTerm: forward a PTY modem-control/break ioctl to the usb-serial bridge.\n'
        ' * Returns true if handled (caller PR_void\'s the syscall with result 0). */\n'
        'static bool maybe_proxy_modem(Tracee *tracee, word_t cmd, word_t fd, word_t arg)\n'
        '{\n'
        '\tchar link[64], path[PATH_MAX], req[PATH_MAX + 64], resp[64];\n'
        '\tint bits = 0;\n'
        '\tssize_t pl;\n'
        '\n'
        '\tsnprintf(link, sizeof(link), "/proc/%d/fd/%d", tracee->pid, (int) fd);\n'
        '\tpl = readlink(link, path, sizeof(path) - 1);\n'
        '\tif (pl <= 0)\n'
        '\t\treturn false;\n'
        '\tpath[pl] = \'\\0\';\n'
        '\tif (strncmp(path, "/dev/pts/", 9) != 0)\t/* our /dev/ttyUSB* are PTY slaves */\n'
        '\t\treturn false;\n'
        '\n'
        '\tswitch (cmd) {\n'
        '\tcase TIOCMGET:\n'
        '\t\tsnprintf(req, sizeof(req), "GET %s\\n", path);\n'
        '\t\tif (!uknl_modem_call(req, resp, sizeof(resp)) || strncmp(resp, "NAK", 3) == 0)\n'
        '\t\t\treturn false;\n'
        '\t\tbits = atoi(resp);\n'
        '\t\treturn write_data(tracee, arg, &bits, sizeof(bits)) >= 0;\n'
        '\tcase TIOCMSET:\n'
        '\tcase TIOCMBIS:\n'
        '\tcase TIOCMBIC:\n'
        '\t\tif (read_data(tracee, &bits, arg, sizeof(bits)) < 0)\n'
        '\t\t\treturn false;\n'
        '\t\tsnprintf(req, sizeof(req), "%s %s %d\\n",\n'
        '\t\t\tcmd == TIOCMSET ? "SET" : cmd == TIOCMBIS ? "BIS" : "BIC", path, bits);\n'
        '\t\treturn uknl_modem_call(req, resp, sizeof(resp)) && strncmp(resp, "NAK", 3) != 0;\n'
        '\tcase TIOCSBRK:\n'
        '\tcase TIOCCBRK:\n'
        '\t\tsnprintf(req, sizeof(req), "BRK %s %d\\n", path, cmd == TIOCSBRK ? 1 : 0);\n'
        '\t\treturn uknl_modem_call(req, resp, sizeof(resp)) && strncmp(resp, "NAK", 3) != 0;\n'
        '\tcase TCSBRK:\n'
        '\t\tsnprintf(req, sizeof(req), "BRK %s p\\n", path);\n'
        '\t\treturn uknl_modem_call(req, resp, sizeof(resp)) && strncmp(resp, "NAK", 3) != 0;\n'
        '\tdefault:\n'
        '\t\treturn false;\n'
        '\t}\n'
        '}\n\n')
    anchor_fn = 'static int translate_path2(Tracee *tracee, int dir_fd, char path[PATH_MAX], Reg reg, Type type)\n{'
    must(anchor_fn in s, "enter.c translate_path2 fn anchor")
    s = s.replace(anchor_fn, func + anchor_fn, 1)
    # (a) open/stat redirect inside translate_path2, right after the NULL check
    anchor_null = ('\t/* Special case where the argument was NULL. */\n'
                   '\tif (path[0] == \'\\0\')\n'
                   '\t\treturn 0;')
    must(anchor_null in s, "enter.c translate_path2 NULL check anchor")
    s = s.replace(anchor_null, anchor_null +
                  ('\n\n\t/* NeoTerm: /dev/ttyUSB<n> -> its current live PTY (virtual hotplug port). */\n'
                   '\tif (strncmp(path, "/dev/ttyUSB", 11) == 0)\n'
                   '\t\tuknl_ttyusb_redirect(path);\n'
                   '\t/* NeoTerm: /dev/iio:device<n> -> its live PTY (buffered IIO sample stream). */\n'
                   '\telse if (strncmp(path, "/dev/iio:device", 15) == 0)\n'
                   '\t\tuknl_iio_redirect(path);'), 1)
    # (b) modem-control ioctl proxy, after the SIOCGIFINDEX fake
    anchor_case = ('\t\tif (cmd == SIOCGIFINDEX && maybe_fake_siocgifindex(tracee, cmd, arg)) {\n'
                   '\t\t\tpoke_reg(tracee, SYSARG_RESULT, 0);\n'
                   '\t\t\tset_sysnum(tracee, PR_void);\n'
                   '\t\t\tbreak;\n'
                   '\t\t}')
    must(anchor_case in s, "enter.c siocgifindex case anchor")
    new_case = anchor_case + (
        '\n\n\t\tif ((cmd == TIOCMGET || cmd == TIOCMSET || cmd == TIOCMBIS || cmd == TIOCMBIC\n'
        '\t\t     || cmd == TIOCSBRK || cmd == TIOCCBRK || cmd == TCSBRK)\n'
        '\t\t    && maybe_proxy_modem(tracee, cmd, peek_reg(tracee, CURRENT, SYSARG_1), arg)) {\n'
        '\t\t\tpoke_reg(tracee, SYSARG_RESULT, 0);\n'
        '\t\t\tset_sysnum(tracee, PR_void);\n'
        '\t\t\tbreak;\n'
        '\t\t}')
    s = s.replace(anchor_case, new_case, 1)
    wr(EN, s)

# ---- extension/hidden_files/hidden_files.c: inject the active /dev/ttyUSB*
#      entries at the EOF of a getdents on /dev, so glob (ls /dev/ttyUSB*) and
#      enumeration find the virtual hotplug ports. Reuses this extension's
#      linux_dirent64 + readlink_proc_pid_fd; only acts on the /dev directory. ----
HF = ROOT + "/extension/hidden_files/hidden_files.c"; s = rd(HF)
if 'uknl_maybe_inject_ttyusb' not in s:
    must('#include "path/path.h"' in s, "hidden_files path.h include")
    s = s.replace('#include "path/path.h"',
                  '#include "path/path.h"\n'
                  '#include <sys/socket.h>\n#include <sys/un.h>\n#include <sys/time.h>\n'
                  '#include <unistd.h>\n#include <string.h>\n#include <stddef.h>\n'
                  '#include <stdbool.h>\n', 1)
    helpers = (
        '/* NeoTerm: one request/reply to the app-side usb-serial server. */\n'
        'static bool uknl_query(const char *req, char *resp, size_t resp_sz)\n'
        '{\n'
        '\tint s = socket(AF_UNIX, SOCK_STREAM, 0);\n'
        '\tif (s < 0)\n\t\treturn false;\n'
        '\tstruct timeval tv = { .tv_sec = 1, .tv_usec = 0 };\n'
        '\tsetsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);\n'
        '\tsetsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);\n'
        '\tstruct sockaddr_un a;\n\tmemset(&a, 0, sizeof a);\n\ta.sun_family = AF_UNIX;\n'
        '\tconst char *name = "io.neoterm.ttyusb";\n'
        '\ta.sun_path[0] = \'\\0\';\n'
        '\tstrncpy(a.sun_path + 1, name, sizeof(a.sun_path) - 2);\n'
        '\tsocklen_t len = sizeof(a.sun_family) + 1 + strlen(name);\n'
        '\tif (connect(s, (struct sockaddr *) &a, len) < 0) {\n\t\tclose(s);\n\t\treturn false;\n\t}\n'
        '\tsize_t rl = strlen(req);\n'
        '\tbool ok = write(s, req, rl) == (ssize_t) rl;\n'
        '\tssize_t n = ok ? read(s, resp, resp_sz - 1) : -1;\n'
        '\tclose(s);\n'
        '\tif (n <= 0)\n\t\treturn false;\n'
        '\tresp[n] = \'\\0\';\n'
        '\twhile (n > 0 && (resp[n - 1] == \'\\n\' || resp[n - 1] == \'\\r\'))\n\t\tresp[--n] = \'\\0\';\n'
        '\treturn true;\n}\n\n'
        '/* per-(pid,fd) "already injected at EOF" flags */\n'
        '#define UKNL_MAXSET 64\n'
        'static struct { int pid; int fd; } uknl_inj[UKNL_MAXSET];\n'
        'static int uknl_inj_n = 0;\n'
        'static bool uknl_inj_has(int pid, int fd)\n{\n'
        '\tfor (int i = 0; i < uknl_inj_n; i++)\n\t\tif (uknl_inj[i].pid == pid && uknl_inj[i].fd == fd) return true;\n'
        '\treturn false;\n}\n'
        'static void uknl_inj_add(int pid, int fd)\n{\n'
        '\tif (uknl_inj_has(pid, fd)) return;\n'
        '\tif (uknl_inj_n < UKNL_MAXSET) { uknl_inj[uknl_inj_n].pid = pid; uknl_inj[uknl_inj_n].fd = fd; uknl_inj_n++; }\n}\n'
        'static void uknl_inj_del(int pid, int fd)\n{\n'
        '\tfor (int i = 0; i < uknl_inj_n; i++)\n\t\tif (uknl_inj[i].pid == pid && uknl_inj[i].fd == fd) { uknl_inj[i] = uknl_inj[--uknl_inj_n]; return; }\n}\n\n'
        'static size_t uknl_put_dirent64(char *buf, size_t avail, const char *name,\n'
        '\t\tunsigned long long ino, long long off, unsigned char dtype)\n{\n'
        '\tsize_t nl = strlen(name);\n'
        '\tsize_t reclen = (offsetof(struct linux_dirent64, d_name) + nl + 1 + 7) & ~(size_t) 7;\n'
        '\tif (reclen > avail) return 0;\n'
        '\tstruct linux_dirent64 *d = (struct linux_dirent64 *) buf;\n'
        '\td->d_ino = ino;\n\td->d_off = off;\n\td->d_reclen = (unsigned short) reclen;\n'
        '\td->d_type = dtype;\n'
        '\tmemcpy(d->d_name, name, nl + 1);\n'
        '\tfor (size_t i = offsetof(struct linux_dirent64, d_name) + nl + 1; i < reclen; i++) buf[i] = 0;\n'
        '\treturn reclen;\n}\n\n'
        '/* Append the active ttyUSB ports at the EOF of a getdents64 on /dev, so\n'
        ' * glob (ls /dev/ttyUSB*) and enumeration find the virtual hotplug ports. */\n'
        'static bool uknl_maybe_inject_ttyusb(Tracee *tracee, int res)\n{\n'
        '\tif (get_sysnum(tracee, ORIGINAL) != PR_getdents64)\n\t\treturn false;\n'
        '\tint fd = (int) peek_reg(tracee, ORIGINAL, SYSARG_1);\n'
        '\tchar path[PATH_MAX];\n'
        '\tif (readlink_proc_pid_fd(tracee->pid, fd, path) < 0)\n\t\treturn false;\n'
        '\tunsigned char dtype = 2;\t/* DT_CHR */\n'
        '\tif (strcmp(path, "/dev") != 0)\n\t\treturn false;\n'
        '\tif (res > 0) { uknl_inj_del(tracee->pid, fd); return false; }\n'
        '\tif (res != 0)\n\t\treturn false;\n'
        '\tif (uknl_inj_has(tracee->pid, fd))\n\t\treturn false;\n'
        '\tchar list[256];\n'
        '\tif (!uknl_query("LIST\\n", list, sizeof list)) return false;\n'
        '\tif (list[0] == \'\\0\' || list[0] == \'-\') { uknl_inj_add(tracee->pid, fd); return false; }\n'
        '\tword_t buf_addr = peek_reg(tracee, ORIGINAL, SYSARG_2);\n'
        '\tunsigned int count = peek_reg(tracee, ORIGINAL, SYSARG_3);\n'
        '\tchar out[1024];\n\tsize_t off = 0;\n\tunsigned long long ino = 0x7574620000ULL;\n'
        '\tchar *save = NULL;\n'
        '\tfor (char *tok = strtok_r(list, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {\n'
        '\t\tif (tok[0] == \'\\0\') continue;\n'
        '\t\tsize_t r = uknl_put_dirent64(out + off, sizeof(out) - off, tok, ino++, (long long)(0x7f000000 + off), dtype);\n'
        '\t\tif (r == 0) break;\n\t\toff += r;\n\t}\n'
        '\tif (off == 0 || off > count) { uknl_inj_add(tracee->pid, fd); return false; }\n'
        '\tif (write_data(tracee, buf_addr, out, off) < 0) return false;\n'
        '\tpoke_reg(tracee, SYSARG_RESULT, off);\n'
        '\tuknl_inj_add(tracee->pid, fd);\n'
        '\treturn true;\n}\n\n')
    anchor_hf = 'static int handle_getdents(Tracee *tracee)\n{'
    must(anchor_hf in s, "hidden_files handle_getdents anchor")
    s = s.replace(anchor_hf, helpers + anchor_hf, 1)
    anchor_res = ('        unsigned int res = peek_reg(tracee, CURRENT, SYSARG_RESULT);\n'
                  '        if (res <= 0) {')
    must(anchor_res in s, "hidden_files res anchor")
    s = s.replace(anchor_res,
                  '        unsigned int res = peek_reg(tracee, CURRENT, SYSARG_RESULT);\n'
                  '        if (uknl_maybe_inject_ttyusb(tracee, (int) res))\n            return 0;\n'
                  '        if (res <= 0) {', 1)
    wr(HF, s)

# ---- flock(2) on a devpts node is denied by Android SELinux (EACCES), which
#      breaks serial tools that open the port with exclusive locking (pyserial
#      exclusive=True / modern esptool). Trap flock and fake success for
#      /dev/pts/* fds only; non-pts flock (dpkg locks, etc.) is untouched. ----
SC = ROOT + "/syscall/seccomp.c"; s = rd(SC)
if 'PR_flock' not in s:
    sc_anchor = '\t{ PR_fchdir,\t\tFILTER_SYSEXIT },'
    must(sc_anchor in s, "seccomp fchdir anchor")
    s = s.replace(sc_anchor, sc_anchor + '\n\t{ PR_flock,\t\tFILTER_SYSEXIT },', 1)
    wr(SC, s)

EF = ROOT + "/syscall/enter.c"; s = rd(EF)
if 'NeoTerm: Android SELinux denies flock' not in s:
    fl_anchor = '\tcase PR_ioctl: {'
    must(fl_anchor in s, "enter.c ioctl anchor (flock)")
    fl_case = (
        '\t/* NeoTerm: Android SELinux denies flock(2) on a devpts node (EACCES),\n'
        '\t * breaking serial tools that lock the port exclusively (pyserial\n'
        '\t * exclusive=True / esptool). Fake success for /dev/pts/* fds only. */\n'
        '\tcase PR_flock: {\n'
        '\t\tchar lk[64], pp[PATH_MAX];\n'
        '\t\tsnprintf(lk, sizeof(lk), "/proc/%d/fd/%d", tracee->pid, (int) peek_reg(tracee, CURRENT, SYSARG_1));\n'
        '\t\tssize_t pn = readlink(lk, pp, sizeof(pp) - 1);\n'
        '\t\tif (pn > 0) {\n'
        '\t\t\tpp[pn] = \'\\0\';\n'
        '\t\t\tif (strncmp(pp, "/dev/pts/", 9) == 0) {\n'
        '\t\t\t\tpoke_reg(tracee, SYSARG_RESULT, 0);\n'
        '\t\t\t\tset_sysnum(tracee, PR_void);\n'
        '\t\t\t}\n'
        '\t\t}\n'
        '\t\tbreak;\n'
        '\t}\n\n')
    s = s.replace(fl_anchor, fl_case + fl_anchor, 1)
    wr(EF, s)

print("ALL PATCHES APPLIED OK")
