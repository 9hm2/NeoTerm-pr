/* Standalone test for the proot VFS-redirect C (patches/uknl_fs_redirect.c).
 *
 * That file is injected into proot's enter.c and uses proot's scope (Tracee,
 * peek/poke_reg, set_sysnum, read_string/write_data, PR_* sysnums, the block
 * proxy's uksd_* socket helpers). Here we provide minimal stubs for all of them
 * and #include the redirect verbatim, which (a) type-checks the whole file under
 * -Wall -Wextra and (b) lets us unit-test the getdents64 synthesis + LIST-blob
 * parsing — the trickiest pure logic — against a canned directory listing.
 *
 * Built and run by test/run_host_tests.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>

typedef unsigned long word_t;
typedef struct { int pid; } Tracee;
typedef enum { CURRENT, ORIGINAL, MODIFIED } RegVersion;
typedef enum { SYSARG_1, SYSARG_2, SYSARG_3, SYSARG_4, SYSARG_5, SYSARG_6, SYSARG_RESULT } Reg;
enum { PR_void = 0, PR_mount, PR_newfstatat, PR_fstatat64, PR_statx,
       PR_openat, PR_openat2, PR_read, PR_pread64, PR_lseek, PR_getdents64,
       PR_close, PR_readlinkat, PR_write, PR_pwrite64, PR_ftruncate,
       PR_mkdirat, PR_unlinkat, PR_symlinkat, PR_fchmodat, PR_fchownat,
       PR_truncate, PR_renameat, PR_renameat2,
       PR_umount2, PR_fstat, PR_getxattr, PR_lgetxattr, PR_listxattr, PR_llistxattr,
       PR_dup, PR_dup2, PR_dup3, PR_access, PR_faccessat, PR_faccessat2,
       PR_fsync, PR_fdatasync };
#ifndef EXDEV
#define EXDEV 18
#endif
#ifndef ENOTSUP
#define ENOTSUP 95
#endif

static word_t peek_reg(Tracee *t, RegVersion v, Reg r) { (void)t;(void)v;(void)r; return 0; }
static void   poke_reg(Tracee *t, Reg r, word_t val) { (void)t;(void)r;(void)val; }
static void   set_sysnum(Tracee *t, int n) { (void)t;(void)n; }
static int    read_string(Tracee *t, char *dst, word_t src, size_t max) { (void)t;(void)src; if (max) dst[0] = 0; return 0; }
static int    write_data(Tracee *t, word_t a, const void *src, size_t n) { (void)t;(void)a;(void)src;(void)n; return 0; }
static int    read_data(Tracee *t, void *dst, word_t src, size_t n) { (void)t;(void)src; memset(dst, 0, n); return 0; }
static int    set_sysarg_path(Tracee *t, const char *p, Reg r) { (void)t;(void)p;(void)r; return 0; }
static int    get_sysarg_path(Tracee *t, char path[PATH_MAX], Reg r) { (void)t;(void)r; if (path) path[0] = 0; return 0; }
/* stubs for the temporary kmsg debug (uk_dbg) */
typedef struct { char path[PATH_MAX]; } Path;
typedef struct { Path host; Path guest; } Binding;
enum { GUEST, HOST };
static Binding *get_binding(Tracee *t, int side, char path[PATH_MAX]) { (void)t;(void)side;(void)path; return 0; }

/* block proxy socket helpers (injected just above the FS block in enter.c). The
 * getdents test drives them through a canned, scriptable response. */
static char           g_resp_line[128];
static unsigned char  g_resp_blob[8192];
static size_t         g_resp_blen, g_resp_bpos;
static int uksd_wn(int s, const void *b, size_t n) { (void)s;(void)b;(void)n; return 0; }
static int uksd_rl(int s, char *b, size_t bs) { (void)s; snprintf(b, bs, "%s", g_resp_line); return (int)strlen(b); }
static int uksd_rn(int s, void *b, size_t n) { (void)s; if (g_resp_bpos + n > g_resp_blen) return -1; memcpy(b, g_resp_blob + g_resp_bpos, n); g_resp_bpos += n; return 0; }

#include "uknl_fs_redirect.c"   /* found via -I <repo>/proot/patches (see run_host_tests.sh) */

/* build one LIST blob entry: {u8 type, u64 ino, u64 size, u16 namelen, name} */
static size_t put_ent(unsigned char *p, int type, unsigned long long ino, unsigned long long sz, const char *nm)
{
    size_t nl = strlen(nm); p[0] = (unsigned char)type;
    memcpy(p + 1, &ino, 8); memcpy(p + 9, &sz, 8);
    unsigned short n = (unsigned short)nl; memcpy(p + 17, &n, 2); memcpy(p + 19, nm, nl);
    return 19 + nl;
}

int main(void)
{
    int fails = 0;
    /* arm a 2-entry LIST: HELLO.TXT (file, ino 5), SUBDIR (dir, ino 7) */
    size_t o = 0;
    o += put_ent(g_resp_blob + o, 2, 5, 24, "HELLO.TXT");
    o += put_ent(g_resp_blob + o, 1, 7, 0,  "SUBDIR");
    g_resp_blen = o; g_resp_bpos = 0;
    snprintf(g_resp_line, sizeof g_resp_line, "OK 2 %zu", o);

    g_ukfs_sock = 3;   /* bypass the real connect(); uksd_* are scripted */
    struct ukfs_vfd v; memset(&v, 0, sizeof v); v.used = 1; v.isdir = 1; strcpy(v.path, "/");

    if (ukfs_load_dir(&v) != 0) { printf("  FAIL  ukfs_load_dir\n"); return 1; }
    /* expect 4 entries: "." ".." HELLO.TXT SUBDIR */
    int ok = (v.dent_n == 4) &&
             !strcmp(v.dents[0].name, ".") && !strcmp(v.dents[1].name, "..") &&
             !strcmp(v.dents[2].name, "HELLO.TXT") && v.dents[2].type == 2 &&
             !strcmp(v.dents[3].name, "SUBDIR") && v.dents[3].type == 1;
    printf(ok ? "  PASS  load_dir parses LIST blob (. .. HELLO.TXT SUBDIR)\n"
              : "  FAIL  load_dir parse\n"); fails += !ok;

    /* emit and re-parse the linux_dirent64 stream */
    unsigned char out[4096]; size_t w = ukfs_emit_dents(&v, out, sizeof out);
    size_t p = 0; int count = 0; const char *names[8]; unsigned types[8]; int bad = 0;
    while (p < w) {
        unsigned short reclen; memcpy(&reclen, out + p + 16, 2);
        if (reclen == 0 || (reclen & 7)) { bad = 1; break; }   /* must be 8-aligned, non-zero */
        if (count < 8) { names[count] = (const char *)(out + p + 19); types[count] = out[p + 18]; }
        p += reclen; count++;
    }
    ok = !bad && count == 4 &&
         !strcmp(names[0], ".") && types[0] == 4 /*DT_DIR*/ &&
         !strcmp(names[2], "HELLO.TXT") && types[2] == 8 /*DT_REG*/ &&
         !strcmp(names[3], "SUBDIR") && types[3] == 4;
    printf(ok ? "  PASS  emit_dents -> valid linux_dirent64 records (DT_DIR/DT_REG)\n"
              : "  FAIL  emit_dents record stream\n"); fails += !ok;

    ok = (ukfs_emit_dents(&v, out, sizeof out) == 0);   /* cursor at end -> EOF */
    printf(ok ? "  PASS  emit_dents returns 0 at EOF\n" : "  FAIL  emit_dents EOF\n"); fails += !ok;

    v.dent_idx = 0;
    ok = (ukfs_emit_dents(&v, out, 8) == 0 && v.dent_idx == 0);  /* too small -> 0, no advance (=> EINVAL path) */
    printf(ok ? "  PASS  emit_dents tiny buffer -> 0, cursor unmoved (EINVAL path)\n"
              : "  FAIL  emit_dents small-buffer\n"); fails += !ok;

    if (fails) { printf("\n%d REDIRECT CHECK(S) FAILED\n", fails); return 1; }
    printf("\nALL REDIRECT TESTS PASSED\n");
    return 0;
}
