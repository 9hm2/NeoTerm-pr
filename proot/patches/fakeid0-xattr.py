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
                  '#include <stdbool.h>\n#include <stdlib.h>\n', 1)
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
        '/* NeoTerm: true iff the io.neoterm.block server has a device attached\n'
        ' * (SIZE -> OK only while deviceName != null), so /dev/uksd0 hotplugs. */\n'
        'static bool uknl_block_present(void)\n{\n'
        '\tint s = socket(AF_UNIX, SOCK_STREAM, 0);\n'
        '\tif (s < 0) return false;\n'
        '\tstruct timeval tv = { .tv_sec = 1, .tv_usec = 0 };\n'
        '\tsetsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);\n'
        '\tsetsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);\n'
        '\tstruct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX;\n'
        '\tconst char *nm = "io.neoterm.block";\n'
        '\ta.sun_path[0] = \'\\0\'; strncpy(a.sun_path + 1, nm, sizeof(a.sun_path) - 2);\n'
        '\tsocklen_t len = sizeof(a.sun_family) + 1 + strlen(nm);\n'
        '\tif (connect(s, (struct sockaddr *) &a, len) < 0) { close(s); return false; }\n'
        '\tbool ok = write(s, "SIZE\\n", 5) == 5;\n'
        '\tchar r[8] = {0}; ssize_t n = ok ? read(s, r, sizeof r - 1) : -1;\n'
        '\tclose(s);\n'
        '\treturn n >= 2 && r[0] == \'O\' && r[1] == \'K\';\n}\n\n'
        '/* NeoTerm: partition indices of the attached USB device (PARTS over\n'
        ' * io.neoterm.block), so /dev/uksd0pN nodes show up in /dev. Returns the count. */\n'
        'static int uknl_block_parts(int *idxs, int max)\n{\n'
        '\tint s = socket(AF_UNIX, SOCK_STREAM, 0);\n'
        '\tif (s < 0) return 0;\n'
        '\tstruct timeval tv = { .tv_sec = 1, .tv_usec = 0 };\n'
        '\tsetsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);\n'
        '\tsetsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);\n'
        '\tstruct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX;\n'
        '\tconst char *nm = "io.neoterm.block";\n'
        '\ta.sun_path[0] = \'\\0\'; strncpy(a.sun_path + 1, nm, sizeof(a.sun_path) - 2);\n'
        '\tsocklen_t len = sizeof(a.sun_family) + 1 + strlen(nm);\n'
        '\tif (connect(s, (struct sockaddr *) &a, len) < 0) { close(s); return 0; }\n'
        '\tif (write(s, "PARTS\\n", 6) != 6) { close(s); return 0; }\n'
        '\tchar buf[1024]; ssize_t n = read(s, buf, sizeof buf - 1); close(s);\n'
        '\tif (n <= 0) return 0; buf[n] = \'\\0\';\n'
        '\tif (buf[0] != \'O\' || buf[1] != \'K\') return 0;\n'
        '\tint cnt = 0; char *save = NULL;\n'
        '\t(void) strtok_r(buf, "\\n", &save);   /* skip the "OK <n>" header line */\n'
        '\tfor (char *line = strtok_r(NULL, "\\n", &save); line && cnt < max; line = strtok_r(NULL, "\\n", &save)) {\n'
        '\t\tint idx; if (sscanf(line, "%d", &idx) == 1 && idx > 0) idxs[cnt++] = idx;\n'
        '\t}\n'
        '\treturn cnt;\n}\n\n'
        '/* Append the active ttyUSB ports (and /dev/uksd0 when USB-storage is on) at\n'
        ' * the EOF of a getdents64 on /dev, so glob (ls /dev/ttyUSB*, ls /dev/uksd0)\n'
        ' * and enumeration find the virtual hotplug nodes. */\n'
        'static bool uknl_maybe_inject_ttyusb(Tracee *tracee, int res)\n{\n'
        '\tif (get_sysnum(tracee, ORIGINAL) != PR_getdents64)\n\t\treturn false;\n'
        '\tint fd = (int) peek_reg(tracee, ORIGINAL, SYSARG_1);\n'
        '\tchar path[PATH_MAX];\n'
        '\tif (readlink_proc_pid_fd(tracee->pid, fd, path) < 0)\n\t\treturn false;\n'
        '\tif (strcmp(path, "/dev") != 0)\n\t\treturn false;\n'
        '\tif (res > 0) { uknl_inj_del(tracee->pid, fd); return false; }\n'
        '\tif (res != 0)\n\t\treturn false;\n'
        '\tif (uknl_inj_has(tracee->pid, fd))\n\t\treturn false;\n'
        '\tword_t buf_addr = peek_reg(tracee, ORIGINAL, SYSARG_2);\n'
        '\tunsigned int count = peek_reg(tracee, ORIGINAL, SYSARG_3);\n'
        '\tchar out[1024];\n\tsize_t off = 0;\n\tunsigned long long ino = 0x7574620000ULL;\n'
        '\tchar list[256];\n'
        '\tif (uknl_query("LIST\\n", list, sizeof list) && list[0] != \'\\0\' && list[0] != \'-\') {\n'
        '\t\tchar *save = NULL;\n'
        '\t\tfor (char *tok = strtok_r(list, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {\n'
        '\t\t\tif (tok[0] == \'\\0\') continue;\n'
        '\t\t\tsize_t r = uknl_put_dirent64(out + off, sizeof(out) - off, tok, ino++, (long long)(0x7f000000 + off), 2 /* DT_CHR */);\n'
        '\t\t\tif (r == 0) break;\n\t\t\toff += r;\n\t\t}\n'
        '\t}\n'
        '\tif (getenv("UK_BLOCK") && uknl_block_present()) {\n'
        '\t\tsize_t r = uknl_put_dirent64(out + off, sizeof(out) - off, "uksd0", ino++, (long long)(0x7f000000 + off), 6 /* DT_BLK */);\n'
        '\t\tif (r > 0) off += r;\n'
        '\t\tint pidx[16]; int pn = uknl_block_parts(pidx, 16);\n'
        '\t\tfor (int i = 0; i < pn; i++) {\n'
        '\t\t\tchar pnm[16]; snprintf(pnm, sizeof pnm, "uksd0p%d", pidx[i]);\n'
        '\t\t\tsize_t rr = uknl_put_dirent64(out + off, sizeof(out) - off, pnm, ino++, (long long)(0x7f000000 + off), 6 /* DT_BLK */);\n'
        '\t\t\tif (rr == 0) break;\n\t\t\toff += rr;\n'
        '\t\t}\n'
        '\t}\n'
        '\tif (getenv("UK_CAM")) {\n'
        '\t\tsize_t r = uknl_put_dirent64(out + off, sizeof(out) - off, "video0", ino++, (long long)(0x7f000000 + off), 2 /* DT_CHR */);\n'
        '\t\tif (r > 0) off += r;\n'
        '\t}\n'
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

# ---- USB block-device proxy: /dev/uksd0 (a bound empty marker) becomes a real
#      block device by proxying its I/O syscalls to the app-side SCSI bridge over
#      io.neoterm.block. dd/fdisk/parted/mkfs.*/fsck.*/mtools + a userspace FS all
#      work on ANY filesystem, no root. Gated by the UK_BLOCK env (set by
#      ProotManager only when the storage toggle is on) so non-storage sessions
#      pay nothing: without it the I/O syscalls are not even in the seccomp filter. ----
EN = ROOT + "/syscall/enter.c"; s = rd(EN)
if 'uknl_block_dispatch' not in s:
    must('int translate_syscall_enter(Tracee *tracee)\n{' in s, "enter.c translate_syscall_enter anchor (block)")
    helpers = (
        '/* NeoTerm: USB block-device proxy (/dev/uksd0 -> io.neoterm.block). */\n'
        'static int g_uk_block = -1;\n'
        'static int uk_block_on(void) { if (g_uk_block < 0) { const char *e = getenv("UK_BLOCK"); g_uk_block = (e && *e && *e != \'0\') ? 1 : 0; } return g_uk_block; }\n'
        'static int g_uksd_sock = -1;\n'
        'static long long g_uksd_size = -1;\n'
        'static int g_uksd_sector = 512;\n'
        'static int uksd_conn(void)\n'
        '{\n'
        '\tif (g_uksd_sock >= 0) return g_uksd_sock;\n'
        '\tint s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);\n'
        '\tif (s < 0) return -1;\n'
        '\tstruct timeval tv = { .tv_sec = 20, .tv_usec = 0 };\n'
        '\tsetsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);\n'
        '\tsetsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);\n'
        '\tstruct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX;\n'
        '\tconst char *name = "io.neoterm.block";\n'
        '\ta.sun_path[0] = \'\\0\'; strncpy(a.sun_path + 1, name, sizeof(a.sun_path) - 2);\n'
        '\tsocklen_t len = sizeof(a.sun_family) + 1 + strlen(name);\n'
        '\tif (connect(s, (struct sockaddr *) &a, len) < 0) { close(s); return -1; }\n'
        '\tg_uksd_sock = s; return s;\n'
        '}\n'
        'static void uksd_drop(void) { if (g_uksd_sock >= 0) { close(g_uksd_sock); g_uksd_sock = -1; } }\n'
        'static int uksd_wn(int s, const void *b, size_t n) { size_t o = 0; while (o < n) { ssize_t r = write(s, (const char *)b + o, n - o); if (r < 0) { if (errno == EINTR) continue; return -1; } if (r == 0) return -1; o += r; } return 0; }\n'
        'static int uksd_rn(int s, void *b, size_t n) { size_t o = 0; while (o < n) { ssize_t r = read(s, (char *)b + o, n - o); if (r < 0) { if (errno == EINTR) continue; return -1; } if (r == 0) return -1; o += r; } return 0; }\n'
        'static int uksd_rl(int s, char *b, size_t bs) { size_t o = 0; while (o + 1 < bs) { char c; if (uksd_rn(s, &c, 1) < 0) return -1; if (c == \'\\n\') { b[o] = 0; return (int)o; } b[o++] = c; } b[o] = 0; return (int)o; }\n'
        'static int uksd_size_q(void)\n'
        '{\n'
        '\tif (g_uksd_size >= 0) return 0;\n'
        '\tint s = uksd_conn(); if (s < 0) return -1;\n'
        '\tif (uksd_wn(s, "SIZE\\n", 5) < 0) { uksd_drop(); return -1; }\n'
        '\tchar line[128]; if (uksd_rl(s, line, sizeof line) < 0) { uksd_drop(); return -1; }\n'
        '\tlong long sz; int sec;\n'
        '\tif (sscanf(line, "OK %lld %d", &sz, &sec) != 2) return -1;\n'
        '\tg_uksd_size = sz; g_uksd_sector = sec; return 0;\n'
        '}\n'
        '/* /dev/uksd0 -> 0 (whole device); /dev/uksd0pN -> N. Accepts the guest path\n'
        ' * or the bound marker host path (…/sysdata/uksd0[pN]). -1 if not a uksd node. */\n'
        'static int uksd_idx_of(const char *p)\n'
        '{\n'
        '\tconst char *b = strrchr(p, \'/\'); b = b ? b + 1 : p;\n'
        '\tif (strcmp(b, "uksd0") == 0) return 0;\n'
        '\tif (strncmp(b, "uksd0p", 6) == 0 && b[6]) {\n'
        '\t\tint n = 0; for (const char *q = b + 6; *q; q++) { if (*q < \'0\' || *q > \'9\') return -1; n = n * 10 + (*q - \'0\'); }\n'
        '\t\treturn n;\n'
        '\t}\n'
        '\treturn -1;\n'
        '}\n'
        '/* The uksd index for an open fd (via /proc/<pid>/fd readlink), or -1. */\n'
        'static int uksd_fd_idx(Tracee *tracee, int fd)\n'
        '{\n'
        '\tchar link[64], path[PATH_MAX];\n'
        '\tsnprintf(link, sizeof link, "/proc/%d/fd/%d", tracee->pid, fd);\n'
        '\tssize_t n = readlink(link, path, sizeof(path) - 1);\n'
        '\tif (n <= 0) return -1; path[n] = \'\\0\';\n'
        '\tif (!strstr(path, "/sysdata/uksd0")) return -1;\n'
        '\treturn uksd_idx_of(path);\n'
        '}\n'
        '/* Geometry of uksd index `idx`: idx 0 = whole device; idx N = partition N,\n'
        ' * looked up LIVE from the app (PARTS) so it tracks re-partitioning. Fills\n'
        ' * *base/*size (bytes) and returns 0, or -1 if no such partition. */\n'
        'static int uksd_geom(int idx, long long *base, long long *size)\n'
        '{\n'
        '\tif (idx <= 0) { if (uksd_size_q() < 0) return -1; *base = 0; *size = g_uksd_size; return 0; }\n'
        '\tint s = uksd_conn(); if (s < 0) return -1;\n'
        '\tif (uksd_wn(s, "PARTS\\n", 6) < 0) { uksd_drop(); return -1; }\n'
        '\tchar line[64]; if (uksd_rl(s, line, sizeof line) < 0) { uksd_drop(); return -1; }\n'
        '\tint nn; if (sscanf(line, "OK %d", &nn) != 1) return -1;\n'
        '\tint found = -1;\n'
        '\tfor (int i = 0; i < nn; i++) {\n'
        '\t\tchar l[96]; if (uksd_rl(s, l, sizeof l) < 0) { uksd_drop(); return -1; }\n'
        '\t\tint pi; long long pb, pz;\n'
        '\t\tif (sscanf(l, "%d %lld %lld", &pi, &pb, &pz) == 3 && pi == idx) { *base = pb; *size = pz; found = 0; }\n'
        '\t}\n'
        '\treturn found;\n'
        '}\n'
        'static ssize_t uksd_read(long long off, void *buf, size_t len)\n'
        '{\n'
        '\tint s = uksd_conn(); if (s < 0) return -1;\n'
        '\tchar req[64]; int rl = snprintf(req, sizeof req, "READ %lld %zu\\n", off, len);\n'
        '\tif (uksd_wn(s, req, rl) < 0) { uksd_drop(); return -1; }\n'
        '\tchar line[64]; if (uksd_rl(s, line, sizeof line) < 0) { uksd_drop(); return -1; }\n'
        '\tsize_t n; if (sscanf(line, "OK %zu", &n) != 1) return -1;\n'
        '\tif (n > len) n = len;\n'
        '\tif (uksd_rn(s, buf, n) < 0) { uksd_drop(); return -1; }\n'
        '\treturn (ssize_t) n;\n'
        '}\n'
        'static ssize_t uksd_write(long long off, const void *buf, size_t len)\n'
        '{\n'
        '\tint s = uksd_conn(); if (s < 0) return -1;\n'
        '\tchar req[64]; int rl = snprintf(req, sizeof req, "WRITE %lld %zu\\n", off, len);\n'
        '\tif (uksd_wn(s, req, rl) < 0 || uksd_wn(s, buf, len) < 0) { uksd_drop(); return -1; }\n'
        '\tchar line[16]; if (uksd_rl(s, line, sizeof line) < 0) { uksd_drop(); return -1; }\n'
        '\treturn (line[0] == \'O\' && line[1] == \'K\') ? (ssize_t) len : -1;\n'
        '}\n'
        'static void uksd_flush(void)\n'
        '{\n'
        '\tint s = uksd_conn(); if (s < 0) return;\n'
        '\tif (uksd_wn(s, "FLUSH\\n", 6) < 0) { uksd_drop(); return; }\n'
        '\tchar line[16]; if (uksd_rl(s, line, sizeof line) < 0) uksd_drop();\n'
        '}\n'
        '/* Per-fd state: byte cursor + the partition geometry (base/size) resolved\n'
        ' * once from the fd\'s marker path, so reads/writes hit the right region. */\n'
        'static struct { int pid; int fd; long long off; long long base; long long size; int resolved; } g_blk[64];\n'
        'static int g_blkn = 0;\n'
        'static int blk_idx(int pid, int fd) { for (int i = 0; i < g_blkn; i++) if (g_blk[i].pid == pid && g_blk[i].fd == fd) return i; return -1; }\n'
        'static int blk_new(int pid, int fd) { int i = blk_idx(pid, fd); if (i < 0) { if (g_blkn >= 64) return -1; i = g_blkn++; g_blk[i].pid = pid; g_blk[i].fd = fd; g_blk[i].off = 0; g_blk[i].base = 0; g_blk[i].size = -1; g_blk[i].resolved = 0; } return i; }\n'
        'static void blk_del(int pid, int fd) { int i = blk_idx(pid, fd); if (i >= 0) g_blk[i] = g_blk[--g_blkn]; }\n'
        '/* Fake a block-device struct stat (aarch64 kernel layout, 128 bytes) so tools\n'
        ' * recognise /dev/uksd0 as a block special device, exactly like a ttyUSB node. */\n'
        'static void uksd_put_stat(Tracee *tracee, word_t addr, long long size, int minor)\n'
        '{\n'
        '\tunsigned char st[128];\n'
        '\tmemset(st, 0, sizeof st);\n'
        '\tunsigned long st_ino = 0xb10c0UL + (unsigned) minor;\n'
        '\tunsigned int  st_mode = 0060660;            /* S_IFBLK | 0660 */\n'
        '\tunsigned int  st_nlink = 1;\n'
        '\tunsigned long st_rdev = (8UL << 8) | (unsigned) minor;  /* major 8, minor=part (sd-like) */\n'
        '\tlong          st_size = (long) size;\n'
        '\tint           st_blksize = g_uksd_sector > 0 ? g_uksd_sector : 512;\n'
        '\tlong          st_blocks = (long)(size / 512);\n'
        '\tmemcpy(st + 8,  &st_ino,     8);\n'
        '\tmemcpy(st + 16, &st_mode,    4);\n'
        '\tmemcpy(st + 20, &st_nlink,   4);\n'
        '\tmemcpy(st + 32, &st_rdev,    8);\n'
        '\tmemcpy(st + 48, &st_size,    8);\n'
        '\tmemcpy(st + 56, &st_blksize, 4);\n'
        '\tmemcpy(st + 64, &st_blocks,  8);\n'
        '\twrite_data(tracee, addr, st, sizeof st);\n'
        '}\n'
        'static void uksd_put_statx(Tracee *tracee, word_t addr, long long size, int minor)\n'
        '{\n'
        '\tunsigned char sx[256];\n'
        '\tmemset(sx, 0, sizeof sx);\n'
        '\tunsigned int  stx_mask = 0x000007ffU;       /* STATX_BASIC_STATS */\n'
        '\tunsigned int  stx_blksize = g_uksd_sector > 0 ? g_uksd_sector : 512;\n'
        '\tunsigned int  stx_nlink = 1;\n'
        '\tunsigned short stx_mode = 0x61B0;           /* S_IFBLK | 0660 */\n'
        '\tunsigned long long stx_ino = 0xb10c0ULL + (unsigned) minor;\n'
        '\tunsigned long long stx_size = (unsigned long long) size;\n'
        '\tunsigned long long stx_blocks = (unsigned long long)(size / 512);\n'
        '\tunsigned int stx_rdev_major = 8, stx_rdev_minor = (unsigned) minor;\n'
        '\tmemcpy(sx + 0,   &stx_mask,       4);\n'
        '\tmemcpy(sx + 4,   &stx_blksize,    4);\n'
        '\tmemcpy(sx + 16,  &stx_nlink,      4);\n'
        '\tmemcpy(sx + 28,  &stx_mode,       2);\n'
        '\tmemcpy(sx + 32,  &stx_ino,        8);\n'
        '\tmemcpy(sx + 40,  &stx_size,       8);\n'
        '\tmemcpy(sx + 48,  &stx_blocks,     8);\n'
        '\tmemcpy(sx + 128, &stx_rdev_major, 4);\n'
        '\tmemcpy(sx + 132, &stx_rdev_minor, 4);\n'
        '\twrite_data(tracee, addr, sx, sizeof sx);\n'
        '}\n'
        '/* Returns true if a /dev/uksd0[pN] I/O syscall was handled (PR_void\'d + poked).\n'
        ' * Whole device = index 0; partition N = /dev/uksd0pN, served at its base off. */\n'
        'static bool uknl_block_dispatch(Tracee *tracee, word_t nr)\n'
        '{\n'
        '\tif (!uk_block_on()) return false;\n'
        '\tlong long base, size;\n'
        '\t/* stat family: report /dev/uksd0[pN] as a block special device. */\n'
        '\tif (nr == PR_fstat || nr == PR_fstat64) {\n'
        '\t\tint idx = uksd_fd_idx(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1));\n'
        '\t\tif (idx < 0) return false;\n'
        '\t\tif (uksd_geom(idx, &base, &size) < 0) { poke_reg(tracee, SYSARG_RESULT, (word_t)(long)-ENOENT); set_sysnum(tracee, PR_void); return true; }\n'
        '\t\tuksd_put_stat(tracee, peek_reg(tracee, CURRENT, SYSARG_2), size, idx);\n'
        '\t\tpoke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true;\n'
        '\t}\n'
        '\tif (nr == PR_newfstatat || nr == PR_fstatat64) {\n'
        '\t\tword_t pa = peek_reg(tracee, CURRENT, SYSARG_2);\n'
        '\t\tchar gp[PATH_MAX]; int idx;\n'
        '\t\tif (pa && read_string(tracee, gp, pa, sizeof gp) > 0 && gp[0])\n'
        '\t\t\tidx = uksd_idx_of(gp);\n'
        '\t\telse\n'
        '\t\t\tidx = uksd_fd_idx(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1));\n'
        '\t\tif (idx < 0) return false;\n'
        '\t\tif (uksd_geom(idx, &base, &size) < 0) { poke_reg(tracee, SYSARG_RESULT, (word_t)(long)-ENOENT); set_sysnum(tracee, PR_void); return true; }\n'
        '\t\tuksd_put_stat(tracee, peek_reg(tracee, CURRENT, SYSARG_3), size, idx);\n'
        '\t\tpoke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true;\n'
        '\t}\n'
        '\tif (nr == PR_statx) {\n'
        '\t\tword_t pa = peek_reg(tracee, CURRENT, SYSARG_2);\n'
        '\t\tchar gp[PATH_MAX]; int idx;\n'
        '\t\tif (pa && read_string(tracee, gp, pa, sizeof gp) > 0 && gp[0])\n'
        '\t\t\tidx = uksd_idx_of(gp);\n'
        '\t\telse\n'
        '\t\t\tidx = uksd_fd_idx(tracee, (int) peek_reg(tracee, CURRENT, SYSARG_1));\n'
        '\t\tif (idx < 0) return false;\n'
        '\t\tif (uksd_geom(idx, &base, &size) < 0) { poke_reg(tracee, SYSARG_RESULT, (word_t)(long)-ENOENT); set_sysnum(tracee, PR_void); return true; }\n'
        '\t\tuksd_put_statx(tracee, peek_reg(tracee, CURRENT, SYSARG_5), size, idx);\n'
        '\t\tpoke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true;\n'
        '\t}\n'
        '\tif (nr != PR_read && nr != PR_write && nr != PR_lseek &&\n'
        '\t    nr != PR_pread64 && nr != PR_pwrite64 && nr != PR_ioctl && nr != PR_close) return false;\n'
        '\tint fd = (int) peek_reg(tracee, CURRENT, SYSARG_1);\n'
        '\tif (nr == PR_close) { if (blk_idx(tracee->pid, fd) >= 0) uksd_flush(); blk_del(tracee->pid, fd); return false; }\n'
        '\tint idx = uksd_fd_idx(tracee, fd);\n'
        '\tif (idx < 0) return false;\n'
        '\tint bi = blk_new(tracee->pid, fd);\n'
        '\tif (bi < 0) return false;\n'
        '\tif (!g_blk[bi].resolved) {\n'
        '\t\tif (uksd_geom(idx, &base, &size) < 0) { poke_reg(tracee, SYSARG_RESULT, (word_t)(long)-ENXIO); set_sysnum(tracee, PR_void); return true; }\n'
        '\t\tg_blk[bi].base = base; g_blk[bi].size = size; g_blk[bi].resolved = 1;\n'
        '\t}\n'
        '\tbase = g_blk[bi].base; size = g_blk[bi].size;\n'
        '\tif (nr == PR_ioctl) {\n'
        '\t\tunsigned long cmd = (unsigned long) peek_reg(tracee, CURRENT, SYSARG_2);\n'
        '\t\tword_t arg = peek_reg(tracee, CURRENT, SYSARG_3);\n'
        '\t\tif (cmd == 0x80081272UL) { unsigned long long v = (unsigned long long) size; write_data(tracee, arg, &v, 8); poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true; }\n'
        '\t\tif (cmd == 0x1260UL) { unsigned long v = (unsigned long)(size / 512); write_data(tracee, arg, &v, sizeof v); poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true; }\n'
        '\t\tif (cmd == 0x1268UL) { int v = g_uksd_sector; write_data(tracee, arg, &v, sizeof v); poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true; }\n'
        '\t\tif (cmd == 0x1261UL) { poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true; }\n'
        '\t\treturn false;\n'
        '\t}\n'
        '\tlong long *offp = &g_blk[bi].off;\n'
        '\tif (nr == PR_lseek) {\n'
        '\t\tlong long arg = (long long)(off_t) peek_reg(tracee, CURRENT, SYSARG_2);\n'
        '\t\tint whence = (int) peek_reg(tracee, CURRENT, SYSARG_3);\n'
        '\t\tlong long no = (whence == 1) ? *offp + arg : (whence == 2) ? size + arg : arg;\n'
        '\t\tif (no < 0) { poke_reg(tracee, SYSARG_RESULT, (word_t)(long)-EINVAL); set_sysnum(tracee, PR_void); return true; }\n'
        '\t\t*offp = no; poke_reg(tracee, SYSARG_RESULT, (word_t) no); set_sysnum(tracee, PR_void); return true;\n'
        '\t}\n'
        '\tlong long pos = (nr == PR_pread64 || nr == PR_pwrite64)\n'
        '\t\t? (long long)(off_t) peek_reg(tracee, CURRENT, SYSARG_4) : *offp;\n'
        '\tsize_t len = (size_t) peek_reg(tracee, CURRENT, SYSARG_3);\n'
        '\tword_t buf = peek_reg(tracee, CURRENT, SYSARG_2);\n'
        '\tif (len == 0) { poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true; }\n'
        '\t/* The guest sees a 0-based device of `size` bytes; clamp + add the base. */\n'
        '\tif (pos >= size) { poke_reg(tracee, SYSARG_RESULT, 0); set_sysnum(tracee, PR_void); return true; }\n'
        '\tif ((long long) len > size - pos) len = (size_t)(size - pos);\n'
        '\tif (len > (8u << 20)) len = 8u << 20;\n'
        '\tvoid *tmp = malloc(len); if (!tmp) return false;\n'
        '\tssize_t r;\n'
        '\tif (nr == PR_read || nr == PR_pread64) {\n'
        '\t\tr = uksd_read(base + pos, tmp, len);\n'
        '\t\tif (r > 0) write_data(tracee, buf, tmp, r);\n'
        '\t} else {\n'
        '\t\tif (read_data(tracee, tmp, buf, len) < 0) { free(tmp); poke_reg(tracee, SYSARG_RESULT, (word_t)(long)-EFAULT); set_sysnum(tracee, PR_void); return true; }\n'
        '\t\tr = uksd_write(base + pos, tmp, len);\n'
        '\t}\n'
        '\tfree(tmp);\n'
        '\tif (r < 0) { poke_reg(tracee, SYSARG_RESULT, (word_t)(long)-EIO); set_sysnum(tracee, PR_void); return true; }\n'
        '\tif (nr == PR_read || nr == PR_write) *offp += r;\n'
        '\tpoke_reg(tracee, SYSARG_RESULT, (word_t) r); set_sysnum(tracee, PR_void); return true;\n'
        '}\n\n')
    s = s.replace('int translate_syscall_enter(Tracee *tracee)\n{',
                  helpers + 'int translate_syscall_enter(Tracee *tracee)\n{', 1)
    # Inject the dispatch BEFORE notify_extensions (the fake_id0 ENTER hook): vmount
    # paths must be handled by our redirect first, otherwise fake_id0 intercepts e.g.
    # a chmod on the FS file and tries to persist ownership as an xattr on the
    # non-existent host-shadow path -> EPERM (git clone "could not write config file").
    nx_anchor = '\tstatus = notify_extensions(tracee, SYSCALL_ENTER_START, 0, 0);'
    must(nx_anchor in s, "enter.c notify_extensions anchor (block)")
    s = s.replace(nx_anchor,
                  '\tsyscall_number = get_sysnum(tracee, ORIGINAL);\n'
                  '\tif (uknl_block_dispatch(tracee, syscall_number))\n\t\treturn 0;\n'
                  + nx_anchor, 1)
    wr(EN, s)

SC = ROOT + "/syscall/seccomp.c"; s = rd(SC)
if 'uk_block_sysnums' not in s:
    sc_anchor = 'status = merge_filtered_sysnums(tracee->ctx, &filtered_sysnums, proot_sysnums);'
    must(sc_anchor in s, "seccomp proot_sysnums merge anchor (block)")
    s = s.replace(sc_anchor, sc_anchor +
                  '\n\t/* NeoTerm: trap the block-I/O syscalls only when /dev/uksd0 is wanted. */\n'
                  '\tif (status >= 0 && getenv("UK_BLOCK")) {\n'
                  '\t\tstatic const FilteredSysnum uk_block_sysnums[] = {\n'
                  '\t\t\t{ PR_read,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_write,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_pread64,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_pwrite64,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_lseek,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_close,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_ioctl,\tFILTER_SYSEXIT },\t/* loop device emulation (losetup) */\n'
                  '\t\t\tFILTERED_SYSNUM_END,\n'
                  '\t\t};\n'
                  '\t\tstatus = merge_filtered_sysnums(tracee->ctx, &filtered_sysnums, uk_block_sysnums);\n'
                  '\t}', 1)
    wr(SC, s)

# ---- syscall/enter.c: USB-filesystem redirect (/dev/uksd0 mount -> ukfsd over
#      io.neoterm.fs). The C lives in patches/uknl_fs_redirect.c (lintable on its
#      own); we inject it verbatim just before translate_syscall_enter (after the
#      block proxy, so it can reuse uksd_wn/uksd_rn/uksd_rl) and add a dispatch
#      call right after the block dispatch in the syscall switch. ----
import os
EN = ROOT + "/syscall/enter.c"; s = rd(EN)
if 'uknl_fs_dispatch' not in s:
    _pd = os.path.dirname(os.path.abspath(__file__))
    # FUSE engine (phase 2/3): the redirect calls fused_* and references fused_t /
    # the FUSE UAPI. fused.c is plain userspace C; inject it (preceded by the
    # vendored kernel UAPI + the fused API header) right before the redirect, the
    # same verbatim-paste mechanism. Strip fused.c's local #includes (their content
    # is pasted just above) so they need no -I path when compiled inside enter.c.
    fuse_uapi = rd(os.path.join(_pd, "fuse_kernel.h"))
    fused_h   = rd(os.path.join(_pd, "fused.h"))
    fused_c   = rd(os.path.join(_pd, "fused.c"))
    fused_c   = fused_c.replace('#include "fused.h"\n', '').replace('#include "fuse_kernel.h"\n', '')
    fuse_blob = fuse_uapi + "\n" + fused_h + "\n" + fused_c + "\n"
    fs_c = fuse_blob + rd(os.path.join(_pd, "uknl_fs_redirect.c"))
    must('int translate_syscall_enter(Tracee *tracee)\n{' in s, "enter.c translate_syscall_enter anchor (fs)")
    s = s.replace('int translate_syscall_enter(Tracee *tracee)\n{',
                  fs_c + '\nint translate_syscall_enter(Tracee *tracee)\n{', 1)
    blk_anchor = ('\tif (uknl_block_dispatch(tracee, syscall_number))\n\t\treturn 0;\n'
                  '\tstatus = notify_extensions(tracee, SYSCALL_ENTER_START, 0, 0);')
    must(blk_anchor in s, "enter.c block-dispatch anchor (fs)")
    s = s.replace(blk_anchor,
                  '\tif (uknl_block_dispatch(tracee, syscall_number))\n\t\treturn 0;\n'
                  '\tif (uknl_fs_dispatch(tracee, syscall_number))\n\t\treturn 0;\n'
                  '\tstatus = notify_extensions(tracee, SYSCALL_ENTER_START, 0, 0);', 1)
    # mount(2) is blocked by Android's parent seccomp (SIGSYS), so it never reaches
    # translate_syscall_enter — proot's SIGSYS handler calls apply_emulated_mount()
    # directly (tracee/seccomp.c). Hook that common function so the /dev/uksd0
    # mount redirect runs on BOTH paths. Forward-declare the hook (it's defined
    # later, in the injected fs block) and call it at the top of the function.
    am_anchor = 'void apply_emulated_mount(Tracee *tracee)\n{'
    must(am_anchor in s, "enter.c apply_emulated_mount anchor (fs)")
    s = s.replace(am_anchor,
                  'static bool uknl_fs_mount_hook(Tracee *tracee);\n'
                  'void apply_emulated_mount(Tracee *tracee)\n{\n'
                  '\tif (uknl_fs_mount_hook(tracee)) return;', 1)
    # umount(2)/umount2(2) are SIGSYS-blocked on Android too, handled via
    # apply_emulated_umount(); hook it the same way so /mnt/usb2 unmount tears
    # down the vmount + ukfsd connection.
    aum_anchor = 'void apply_emulated_umount(Tracee *tracee)\n{'
    must(aum_anchor in s, "enter.c apply_emulated_umount anchor (fs)")
    s = s.replace(aum_anchor,
                  'static bool uknl_fs_umount_hook(Tracee *tracee);\n'
                  'void apply_emulated_umount(Tracee *tracee)\n{\n'
                  '\tif (uknl_fs_umount_hook(tracee)) return;', 1)
    wr(EN, s)

# ---- syscall/enter.c: V4L2 camera redirect (/dev/video0 -> io.neoterm.camera).
#      The C lives in patches/uknl_cam_redirect.c; inject it just before
#      translate_syscall_enter (after the fs redirect, so it reuses the block
#      proxy's uksd_wn/rn/rl socket helpers) and call its dispatch right after
#      the fs dispatch in the syscall switch. ----
EN = ROOT + "/syscall/enter.c"; s = rd(EN)
if 'uknl_cam_dispatch' not in s:
    _pd = os.path.dirname(os.path.abspath(__file__))
    cam_c = rd(os.path.join(_pd, "uknl_cam_redirect.c"))
    must('int translate_syscall_enter(Tracee *tracee)\n{' in s, "enter.c translate_syscall_enter anchor (cam)")
    s = s.replace('int translate_syscall_enter(Tracee *tracee)\n{',
                  cam_c + '\nint translate_syscall_enter(Tracee *tracee)\n{', 1)
    cam_anchor = ('\tif (uknl_fs_dispatch(tracee, syscall_number))\n\t\treturn 0;\n'
                  '\tstatus = notify_extensions(tracee, SYSCALL_ENTER_START, 0, 0);')
    must(cam_anchor in s, "enter.c fs-dispatch anchor (cam)")
    s = s.replace(cam_anchor,
                  '\tif (uknl_fs_dispatch(tracee, syscall_number))\n\t\treturn 0;\n'
                  '\tif (uknl_cam_dispatch(tracee, syscall_number))\n\t\treturn 0;\n'
                  '\tstatus = notify_extensions(tracee, SYSCALL_ENTER_START, 0, 0);', 1)
    wr(EN, s)

# ---- syscall/enter.c: USB (libusb) enablement — fake the netlink hotplug bind so
#      stock libusb_init() succeeds. The C lives in patches/uknl_usb_redirect.c;
#      inject it before translate_syscall_enter and call its dispatch right after
#      the camera dispatch. ----
EN = ROOT + "/syscall/enter.c"; s = rd(EN)
if 'uknl_usb_dispatch' not in s:
    _pd = os.path.dirname(os.path.abspath(__file__))
    usb_c = rd(os.path.join(_pd, "uknl_usb_redirect.c"))
    must('int translate_syscall_enter(Tracee *tracee)\n{' in s, "enter.c translate_syscall_enter anchor (usb)")
    s = s.replace('int translate_syscall_enter(Tracee *tracee)\n{',
                  usb_c + '\nint translate_syscall_enter(Tracee *tracee)\n{', 1)
    usb_anchor = ('\tif (uknl_cam_dispatch(tracee, syscall_number))\n\t\treturn 0;\n'
                  '\tstatus = notify_extensions(tracee, SYSCALL_ENTER_START, 0, 0);')
    must(usb_anchor in s, "enter.c cam-dispatch anchor (usb)")
    s = s.replace(usb_anchor,
                  '\tif (uknl_cam_dispatch(tracee, syscall_number))\n\t\treturn 0;\n'
                  '\tif (uknl_usb_dispatch(tracee, syscall_number))\n\t\treturn 0;\n'
                  '\tstatus = notify_extensions(tracee, SYSCALL_ENTER_START, 0, 0);', 1)
    wr(EN, s)

# ---- syscall/seccomp.c: trap bind() only when UK_USB is set (libusb netlink). ----
SC = ROOT + "/syscall/seccomp.c"; s = rd(SC)
if 'uk_usb_sysnums' not in s:
    sc_anchor = 'status = merge_filtered_sysnums(tracee->ctx, &filtered_sysnums, proot_sysnums);'
    must(sc_anchor in s, "seccomp proot_sysnums merge anchor (usb)")
    s = s.replace(sc_anchor, sc_anchor +
                  '\n\t/* NeoTerm: trap bind() only when libusb support is wanted. */\n'
                  '\tif (status >= 0 && getenv("UK_USB")) {\n'
                  '\t\tstatic const FilteredSysnum uk_usb_sysnums[] = {\n'
                  '\t\t\t{ PR_socket,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_bind,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_setsockopt,\tFILTER_SYSEXIT },\n'
                  '\t\t\tFILTERED_SYSNUM_END,\n'
                  '\t\t};\n'
                  '\t\tstatus = merge_filtered_sysnums(tracee->ctx, &filtered_sysnums, uk_usb_sysnums);\n'
                  '\t}', 1)
    wr(SC, s)

# ---- syscall/seccomp.c: trap the camera I/O syscalls only when UK_CAM is set. ----
SC = ROOT + "/syscall/seccomp.c"; s = rd(SC)
if 'uk_cam_sysnums' not in s:
    sc_anchor = 'status = merge_filtered_sysnums(tracee->ctx, &filtered_sysnums, proot_sysnums);'
    must(sc_anchor in s, "seccomp proot_sysnums merge anchor (cam)")
    s = s.replace(sc_anchor, sc_anchor +
                  '\n\t/* NeoTerm: trap the V4L2 camera syscalls only when /dev/video0 is wanted. */\n'
                  '\tif (status >= 0 && getenv("UK_CAM")) {\n'
                  '\t\tstatic const FilteredSysnum uk_cam_sysnums[] = {\n'
                  '\t\t\t{ PR_ioctl,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_read,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_pread64,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_close,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_fstat,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_poll,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_ppoll,\tFILTER_SYSEXIT },\n'
                  '\t\t\tFILTERED_SYSNUM_END,\n'
                  '\t\t};\n'
                  '\t\tstatus = merge_filtered_sysnums(tracee->ctx, &filtered_sysnums, uk_cam_sysnums);\n'
                  '\t}', 1)
    wr(SC, s)

# ---- tracee/event.c: a re-entrant "pump the event loop until fd readable" used
#      by the FUSE redirect. A guest libfuse daemon's /dev/fuse read()/write()
#      trap at sysexit (they're in the FS seccomp filter), so the single-threaded
#      tracer must NOT block in read(channel) while serving an accessor — it would
#      deadlock the daemon (stopped at its read-exit, waiting to be restarted).
#      Instead fused calls this to service OTHER tracees' stops (the daemon's in
#      particular) until the reply lands on the channel. It mirrors event_loop()'s
#      per-stop handling exactly. ----
EV = ROOT + "/tracee/event.c"; s = rd(EV)
if 'uknl_pump_one' not in s:
    must('int event_loop()\n{' in s, "event.c event_loop anchor (fuse pump)")
    pump = (
        'extern int uknl_fuse_take_park(int pid);   /* FUSE: park a daemon read */\n'
        'extern void uknl_fuse_drain_parked(void);  /* FUSE: EOF all parked daemons */\n'
        'extern void uknl_fuse_reap_signaled(void); /* FUSE: EOF signalled parked daemons */\n'
        'extern void uknl_fuse_owner_gone(int pid);  /* FUSE: app exited -> drop its mount */\n'
        'extern void uknl_fs_pid_gone(int pid);      /* FS: reap exited pid\'s vfds/loop fds */\n'
        'extern int uknl_fuse_is_daemon(int pid);   /* FUSE: pid owns a channel? */\n'
        '\n'
        '/* Deferred tracee stops. While the re-entrant FUSE pump (uknl_pump_one) is\n'
        ' * advancing the daemon so an accessor gets its reply, every NON-daemon stop\n'
        ' * waitpid() returns is stashed here verbatim and replayed by event_loop(),\n'
        ' * so proot\'s process/wait/zombie emulation sees those events exactly as if it\n'
        ' * had waited them itself. Without this, consuming e.g. a sibling process\'s\n'
        ' * exit inside the pump loses the guest parent\'s wait(2) wakeup and it hangs.\n'
        ' * Each stashed tracee is left stopped (not restarted), so it has at most one\n'
        ' * pending event and per-tracee ordering is preserved. */\n'
        '#define UK_STASH_MAX 512\n'
        'static struct { pid_t pid; int status; } uk_stash[UK_STASH_MAX];\n'
        'static int uk_stash_n = 0;\n'
        'static int uknl_stash_push(pid_t pid, int status)\n{\n'
        '\tif (uk_stash_n >= UK_STASH_MAX) return -1;\n'
        '\tuk_stash[uk_stash_n].pid = pid; uk_stash[uk_stash_n].status = status; uk_stash_n++;\n'
        '\treturn 0;\n}\n'
        'static int uknl_stash_pop(pid_t *pid, int *status)\n{\n'
        '\tif (uk_stash_n == 0) return 0;\n'
        '\t*pid = uk_stash[0].pid; *status = uk_stash[0].status;\n'
        '\tmemmove(&uk_stash[0], &uk_stash[1], (size_t)(--uk_stash_n) * sizeof uk_stash[0]);\n'
        '\treturn 1;\n}\n'
        '\n'
        '/* NeoTerm FUSE: advance the FUSE daemon by ONE stop so an accessor waiting on\n'
        ' * a reply makes progress. Any other tracee that stops meanwhile is stashed for\n'
        ' * event_loop() (see above) rather than handled here. Returns 0, or -1 if no\n'
        ' * children remain. */\n'
        'int uknl_pump_one(void);\n'
        'int uknl_pump_one(void)\n{\n'
        '\tint tracee_status;\n'
        '\tpid_t pid;\n'
        '\tfree_terminated_tracees();\n'
        '\tfor (;;) {\n'
        '\t\tpid = waitpid(-1, &tracee_status, __WALL);\n'
        '\t\tif (pid < 0) { if (errno == EINTR) continue; return -1; }\n'
        '\t\t/* Only the daemon is advanced here; defer everyone else. */\n'
        '\t\tif (!uknl_fuse_is_daemon((int) pid)) {\n'
        '\t\t\tif (uknl_stash_push(pid, tracee_status) == 0) continue;  /* deferred */\n'
        '\t\t\t/* stash full: fall through and handle inline (degraded, rare) */\n'
        '\t\t}\n'
        '\t\tbreak;\n'
        '\t}\n'
        '\tTracee *t = get_tracee(NULL, pid, true);\n'
        '\tif (t == NULL) return 0;\n'
        '\tt->running = false;\n'
        '\tif (notify_extensions(t, NEW_STATUS, tracee_status, 0) != 0) return 0;\n'
        '\tif (t->as_ptracee.ptracer != NULL) { if (handle_ptracee_event(t, tracee_status)) return 0; }\n'
        '\tint signal = handle_tracee_event(t, tracee_status);\n'
        '\tif (uknl_fuse_take_park((int) pid)) return 0;   /* parked daemon read: leave stopped */\n'
        '\t(void) restart_tracee(t, signal);\n'
        '\treturn 0;\n}\n\n'
    )
    s = s.replace('int event_loop()\n{', pump + 'int event_loop()\n{', 1)
    # Main loop: replay any stops the FUSE pump deferred, before waiting for new ones.
    wait_anchor = ('\t\t/* Wait for the next tracee\'s stop. */\n'
                   '\t\tpid = waitpid(-1, &tracee_status, __WALL);')
    must(wait_anchor in s, "event.c main-loop waitpid anchor (fuse stash)")
    s = s.replace(wait_anchor,
                  '\t\t/* Wait for the next tracee\'s stop. FUSE: replay a stop the daemon\n'
                  '\t\t * pump deferred (preserves wait/zombie semantics) before waiting anew. */\n'
                  '\t\tif (!uknl_stash_pop(&pid, &tracee_status))\n'
                  '\t\t\tpid = waitpid(-1, &tracee_status, __WALL);', 1)
    # Main loop: skip restart for a parked FUSE daemon read (resumed later by fch_send).
    ml_anchor = ('\t\tsignal = handle_tracee_event(tracee, tracee_status);\n'
                 '\t\t(void) restart_tracee(tracee, signal);')
    must(ml_anchor in s, "event.c main-loop restart anchor (fuse park)")
    s = s.replace(ml_anchor,
                  '\t\tsignal = handle_tracee_event(tracee, tracee_status);\n'
                  '\t\t/* FUSE: a tracee exited. If it was the session leader (vpid 1), EOF\n'
                  '\t\t * every parked daemon so proot can reach ECHILD and exit. Otherwise\n'
                  '\t\t * (e.g. the app was ^C\'d) release any parked daemon that now has a\n'
                  '\t\t * pending fatal signal, so its mount is torn down instead of leaking. */\n'
                  '\t\tif (WIFEXITED(tracee_status) || WIFSIGNALED(tracee_status)) {\n'
                  '\t\t\tuknl_fs_pid_gone((int) pid);   /* reap the exited pid\'s vfds/loop fds */\n'
                  '\t\t\tif (tracee->vpid == 1) uknl_fuse_drain_parked();\n'
                  '\t\t\telse { uknl_fuse_owner_gone((int) pid); uknl_fuse_reap_signaled(); }\n'
                  '\t\t}\n'
                  '\t\tif (uknl_fuse_take_park((int) pid)) continue;   /* parked FUSE daemon read */\n'
                  '\t\t(void) restart_tracee(tracee, signal);', 1)
    wr(EV, s)

# ---- tracee/tracee.c: note fork/clone so a FUSE channel follows a daemonizing
#      server to its forked child. Several /dev/fuse mounts share the same marker,
#      so once the parent exits, marker-path adoption can't tell the channels
#      apart; recording the fork at creation time (parent alive) keeps each daemon
#      mapped to its own channel. ----
TC = ROOT + "/tracee/tracee.c"; s = rd(TC)
if 'uknl_fuse_note_fork' not in s:
    nc_anchor = ('\tchild = get_tracee(parent, (pid_t) pid, true);\n'
                 '\tif (child == NULL) {\n'
                 '\t\tnote(parent, WARNING, SYSTEM, "running out of memory");\n'
                 '\t\treturn -ENOMEM;\n'
                 '\t}')
    must(nc_anchor in s, "tracee.c new_child get_tracee anchor (fuse fork)")
    s = s.replace(nc_anchor, nc_anchor +
                  '\n\n\t/* NeoTerm FUSE: keep a daemonizing server\'s channel mapped to its child. */\n'
                  '\t{ extern void uknl_fuse_note_fork(int ppid, int cpid); uknl_fuse_note_fork(parent->pid, (int) pid); }', 1)
    wr(TC, s)

# ---- syscall/exit.c: capture the fd returned by a redirected openat so reads/
#      getdents/lseek on it can be proxied (uknl_fs_open_exit lives in enter.c). ----
EX = ROOT + "/syscall/exit.c"; s = rd(EX)
if 'uknl_fs_open_exit' not in s:
    must('void translate_syscall_exit(Tracee *tracee)\n{' in s, "exit.c translate_syscall_exit anchor (fs)")
    s = s.replace('void translate_syscall_exit(Tracee *tracee)\n{',
                  'extern void uknl_fs_open_exit(Tracee *tracee, word_t nr);\n'
                  'extern void uknl_fs_exit_final(Tracee *tracee, word_t nr);\n\n'
                  'void translate_syscall_exit(Tracee *tracee)\n{', 1)
    ex_anchor = '\tsyscall_number = get_sysnum(tracee, ORIGINAL);'
    must(ex_anchor in s, "exit.c get_sysnum anchor (fs)")
    s = s.replace(ex_anchor, ex_anchor + '\n\tuknl_fs_open_exit(tracee, syscall_number);', 1)
    # TEMP DIAG: also call a hook at the VERY END (after SYSCALL_EXIT_END), where a
    # late fake_id0 result-poke would already be applied — to log the final result
    # git actually receives (re-derive the sysnum; it isn't in scope on every path).
    ex_end = ('\tstatus = notify_extensions(tracee, SYSCALL_EXIT_END, 0, 0);\n'
              '\tif (status < 0)\n'
              '\t\tpoke_reg(tracee, SYSARG_RESULT, (word_t) status);\n}')
    must(ex_end in s, "exit.c SYSCALL_EXIT_END tail anchor (fs)")
    s = s.replace(ex_end, ex_end[:-1] +
                  '\tuknl_fs_exit_final(tracee, get_sysnum(tracee, ORIGINAL));\n}', 1)
    wr(EX, s)

# ---- syscall/seccomp.c: trap the fd-based FS syscalls (no path -> not in proot's
#      default translation set) at sysexit, only when UK_FS is requested. openat is
#      added too so the exit hook above runs to capture the fd. ----
SC = ROOT + "/syscall/seccomp.c"; s = rd(SC)
if 'uk_fs_sysnums' not in s:
    sc_anchor = 'status = merge_filtered_sysnums(tracee->ctx, &filtered_sysnums, proot_sysnums);'
    must(sc_anchor in s, "seccomp proot_sysnums merge anchor (fs)")
    s = s.replace(sc_anchor, sc_anchor +
                  '\n\t/* NeoTerm: trap fd-based FS syscalls (+ openat exit) when UK_FS is wanted. */\n'
                  '\tif (status >= 0 && getenv("UK_FS")) {\n'
                  '\t\tstatic const FilteredSysnum uk_fs_sysnums[] = {\n'
                  '\t\t\t{ PR_openat,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_openat2,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_creat,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_fchmodat2,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_copy_file_range,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_sendfile,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_sendfile64,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_read,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_pread64,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_readv,\tFILTER_SYSEXIT },\t/* FUSE daemon channel I/O */\n'
                  '\t\t\t{ PR_writev,\tFILTER_SYSEXIT },\t/* libfuse replies via writev */\n'
                  '\t\t\t{ PR_lseek,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_close,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_getdents64,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_write,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_pwrite64,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_ftruncate,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_fstat,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_dup,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_dup2,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_dup3,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_fsync,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_fdatasync,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_fchmod,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_fchown,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_mmap,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_mmap2,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_msync,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_fcntl,\tFILTER_SYSEXIT },\n'
                  '\t\t\t{ PR_fcntl64,\tFILTER_SYSEXIT },\n'
                  '\t\t\tFILTERED_SYSNUM_END,\n'
                  '\t\t};\n'
                  '\t\tstatus = merge_filtered_sysnums(tracee->ctx, &filtered_sysnums, uk_fs_sysnums);\n'
                  '\t}', 1)
    wr(SC, s)

# ---- path/proc.c: resolve "/proc/self" + "/proc/thread-self" in readlink_proc's
#      @base to the tracee's pid. fd magic-links reached via a binding whose target
#      contains "self" (we bind /proc/self/fd -> /dev/fd and /proc/self/fd/{0,1,2}
#      -> /dev/std{in,out,err}) otherwise hit atoi("self")==0 -> DEFAULT, so PRoot
#      readlink()s them in its OWN context (wrong pid) -> ENOENT. That breaks shell
#      process substitution ("grep: /dev/fd/63: No such file or directory"). With the
#      pid substituted, the /proc/<pid>/fd/<n> branch returns DONT_CANONICALIZE and
#      the anonymous pipe/socket link is left intact for the tracee to open. ----
PC = ROOT + "/path/proc.c"; s = rd(PC)
if 'NeoTerm: normalize "/proc/self"' not in s:
    pc_anchor = ('\tconst Tracee *known_tracee;\n'
                 '\tchar proc_path[64]; /* 64 > sizeof("/proc//fd/") + 2 * sizeof(#ULONG_MAX) */\n'
                 '\tint status;\n'
                 '\tpid_t pid;\n')
    must(pc_anchor in s, "proc.c readlink_proc decls anchor")
    s = s.replace(pc_anchor,
                  '\tconst Tracee *known_tracee;\n'
                  '\tchar proc_path[64]; /* 64 > sizeof("/proc//fd/") + 2 * sizeof(#ULONG_MAX) */\n'
                  '\tchar base_buf[PATH_MAX];\n'
                  '\tint status;\n'
                  '\tpid_t pid;\n'
                  '\n'
                  '\t/* NeoTerm: normalize "/proc/self" and "/proc/thread-self" in @base to the\n'
                  '\t * tracee\'s numeric pid, so fd magic-links reached via a binding whose target\n'
                  '\t * contains "self" (e.g. -b /proc/self/fd:/dev/fd) are handled by the per-pid\n'
                  '\t * branch below instead of being readlink()\'d in PRoot\'s own context. */\n'
                  '\t{\n'
                  '\t\tconst char *rest = NULL;\n'
                  '\t\tif (strncmp(base, "/proc/self", 10) == 0 && (base[10] == \'/\' || base[10] == \'\\0\'))\n'
                  '\t\t\trest = base + 10;\n'
                  '\t\telse if (strncmp(base, "/proc/thread-self", 17) == 0 && (base[17] == \'/\' || base[17] == \'\\0\'))\n'
                  '\t\t\trest = base + 17;\n'
                  '\t\tif (rest != NULL) {\n'
                  '\t\t\tstatus = snprintf(base_buf, sizeof(base_buf), "/proc/%d%s", tracee->pid, rest);\n'
                  '\t\t\tif (status < 0 || (size_t) status >= sizeof(base_buf))\n'
                  '\t\t\t\treturn -EPERM;\n'
                  '\t\t\tbase = base_buf;\n'
                  '\t\t\tcomparison = compare_paths("/proc", base);\n'
                  '\t\t}\n'
                  '\t}\n', 1)
    wr(PC, s)

# ---- path/binding.c: don't canonicalize "/proc/self" + "/proc/thread-self" host
#      binding targets. new_binding() runs realpath2() on the host path at startup,
#      which resolves "self" to PRoot's OWN pid — pinning -b /proc/self/fd:/dev/fd
#      (and /dev/std{in,out,err}) to PRoot's fds. The guest's /dev/fd/N then maps to
#      PRoot fd N -> ENOENT, breaking shell process substitution. Store the path
#      verbatim so "self" stays per-process and resolves to the tracee. ----
PB = ROOT + "/path/binding.c"; s = rd(PB)
if 'NeoTerm: keep the per-process "self"' not in s:
    bind_anchor = ('\tstatus = realpath2(tracee->reconf.tracee, binding->host.path, host, true);\n'
                   '\tif (status < 0) {\n')
    must(bind_anchor in s, "binding.c new_binding realpath2 anchor")
    s = s.replace(bind_anchor,
                  '\tif (strncmp(host, "/proc/self/", 11) == 0 || strncmp(host, "/proc/thread-self/", 18) == 0) {\n'
                  '\t\t/* NeoTerm: keep the per-process "self" magic; realpath2 would resolve it\n'
                  '\t\t * to PRoot\'s OWN pid, pinning -b /proc/self/fd:/dev/fd (and the std* binds)\n'
                  '\t\t * to PRoot\'s fds. Store verbatim so the tracee resolves "self" to itself;\n'
                  '\t\t * readlink_proc maps it to the tracee pid during canonicalization. */\n'
                  '\t\tif (strlen(host) >= PATH_MAX) status = -ENAMETOOLONG;\n'
                  '\t\telse { strcpy(binding->host.path, host); status = 0; }\n'
                  '\t} else\n'
                  '\tstatus = realpath2(tracee->reconf.tracee, binding->host.path, host, true);\n'
                  '\tif (status < 0) {\n', 1)
    wr(PB, s)

# ---- teach proot the fchmodat2(2) syscall (sysnum 452, arch-agnostic on the ABIs
#      we target). glibc routes fchmodat(...,AT_SYMLINK_NOFOLLOW) through it, so
#      tar/cp -p/rsync use it to restore directory modes; without a PR_ mapping proot
#      runs it natively against the host shadow (-> ENOENT under a vmount). Append the
#      abstract sysnum and wire it into each arch's raw->PR_ table so it traps and the
#      redirect's PR_fchmodat2 handler runs. ----
SNL = ROOT + "/syscall/sysnums.list"; s = rd(SNL)
if 'SYSNUM(fchmodat2)' not in s:
    if not s.endswith('\n'): s += '\n'
    s += 'SYSNUM(fchmodat2)\n'
    wr(SNL, s)
for arch, num in (("x86_64", 452), ("arm64", 452), ("arm", 452), ("i386", 452), ("x32", 452), ("sh4", 452)):
    AH = ROOT + "/syscall/sysnums-%s.h" % arch
    try: s = rd(AH)
    except FileNotFoundError: continue
    if 'PR_fchmodat2' in s: continue
    anchor = '[ %d ] = PR_fchmodat,' % {"x86_64":268,"arm64":53,"arm":333,"i386":306,"x32":268,"sh4":306}[arch]
    if anchor in s:
        s = s.replace(anchor, anchor + '\n\t[ %d ] = PR_fchmodat2,' % num, 1)
        wr(AH, s)

print("ALL PATCHES APPLIED OK")
