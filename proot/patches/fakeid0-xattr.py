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
print("ALL PATCHES APPLIED OK")
