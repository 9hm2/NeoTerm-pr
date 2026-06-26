/* uKernel — VFS futásidejű shim (F3).
 *
 * A valódi kernel-fájlrendszer-driver (vfat.so) ehhez a réteghez linkelődik: a
 * super_block/inode/dentry életciklusát, a buffer_head blokk-olvasást, és a mount-utat
 * (register_filesystem -> get_tree_bdev -> fill_super) szolgáljuk ki userspace-ben.
 * A blokk-I/O egy backenden megy: helyi image (UK_BLK_IMG) VAGY a libukblk BOT-ja.
 *
 * Így a host `mount`/`ls`/`cat` a mi uKernelünkhöz jut (nem a host-kernelhez), és a
 * VALÓDI FAT-driver dolgozik a VALÓS eszközön — pont mint a wifinél a chipdriver. */
#define _GNU_SOURCE
#include <linux/fs.h>
#include <linux/vfs.h>   /* struct kstatfs (ukfs_statfs) */
#include <linux/xattr.h> /* struct xattr_handler, XATTR_REPLACE (ukfs_*xattr) */
#include <linux/posix_acl.h>       /* struct posix_acl, ACL_TYPE_* (POSIX ACL) */
#include <linux/posix_acl_xattr.h> /* posix_acl_from/to_xattr */
#include <linux/fileattr.h>        /* struct file_kattr, FS_*_FL (chattr/lsattr) */
#include <linux/fiemap.h>          /* struct fiemap, fiemap_extent_info (FIEMAP) */
#include <linux/iomap.h>           /* struct iomap_ops (iomap_fiemap szignatúra) */
#include <linux/buffer_head.h>
#include <linux/nls.h>
#include <linux/mount.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/backing-dev-defs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

/* time64_to_tm: a Unix-mp (+offset) lebontása struct tm-be — VALÓDI impl a libc gmtime_r-rel.
 * A FAT/exfat időkódolás (fat_time_unix2fat → time64_to_tm) ezt hívja; a korábbi no-op stub a
 * tm-et érintetlenül hagyta → tm_year=0 → a dátum MINDIG 1980 lett. A fat a libc struct tm-jét
 * látja (tm_year=év-1900, tm_mon 0-11) — pont amit a gmtime_r tölt. */
void time64_to_tm(time64_t totalsecs, int offset, void *result)
{
	time_t t = (time_t)(totalsecs + offset);
	gmtime_r(&t, (struct tm *)result);
}

/* ===== globális egyedek, amiket a fat vár ===== */
struct mnt_idmap { int dummy; };
struct mnt_idmap nop_mnt_idmap;
struct timezone sys_tz;
struct user_namespace { int dummy; };
struct user_namespace init_user_ns;

/* ===== blokk-backend ===== */
static struct dentry *g_api_root;  /* fwd: a uk_mount és az API is állítja */
static int   g_bdev_fd = -1;       /* image-backend fd */
static int   g_bdev_sock = -1;     /* 0 = block-over-socket active, -1 = not */
static unsigned g_blocksize = 512;

/* ===== block-over-socket backend (io.neoterm.block) =====
 * On Android there is no real /dev/uksd0: the USB block device is served by the
 * BlockBridge over the abstract socket io.neoterm.block. When uk_mount is given
 * a "@<name>" devpath, sector I/O is proxied over that socket instead of
 * pread/pwrite on a local fd. The client lives in block_sock.c (separate TU, no
 * fake kernel headers — its libc <sys/socket.h> would otherwise clash with the
 * fake <linux/fs.h> __kernel_fsid_t on glibc). */
extern int       bsock_open(const char *name);
extern void      bsock_close(void);
extern ssize_t   bsock_pread(void *buf, size_t len, off_t off);
extern ssize_t   bsock_pwrite(const void *buf, size_t len, off_t off);
extern long long bsock_size(void);
extern void      bsock_flush(void);
/* the block backend (fd image or socket) is open */
static int bdev_active(void) { return g_bdev_fd >= 0 || g_bdev_sock >= 0; }

/* UK_BDEV_DELAY_US: mesterséges latencia per bdev-I/O (a valós USB-pendrive lassúságának
 * szimulálása image-en — a journal-commit-szál vs fő flush-szál race reprodukálásához gdb-vel). */
static long uk_bdev_delay(void) { static long v=-2; if(v==-2){const char*e=getenv("UK_BDEV_DELAY_US"); v=e?atol(e):0;} return v; }
/* UK_BDEV_FAIL_EVERY: minden N-edik bdev-I/O EIO-t ad (a valós USB-átmeneti-hiba szimulálása image-en
 * — a journal/ext4 HIBAKEZELŐ-útjának crash-reprodukálásához gdb-vel, USB nélkül). */
static int uk_bdev_fail_every(void){ static int v=-2; if(v==-2){const char*e=getenv("UK_BDEV_FAIL_EVERY"); v=e?atoi(e):0;} return v; }
static int g_bdev_iocount = 0;
static int uk_bdev_should_fail(void){ int n=uk_bdev_fail_every(); if(n<=0)return 0; return (++g_bdev_iocount % n)==0; }
/* egyetlen nyers I/O-próba (a hiba-injektálással) */
static ssize_t bdev_pread1(void *buf, size_t len, off_t off)
{ long d=uk_bdev_delay(); if(d>0) usleep(d); if(uk_bdev_should_fail()){errno=EIO;return -1;} if(g_bdev_sock>=0) return bsock_pread(buf,len,off); return pread(g_bdev_fd, buf, len, off); }
static ssize_t bdev_pwrite1(const void *buf, size_t len, off_t off)
{ long d=uk_bdev_delay(); if(d>0) usleep(d); if(uk_bdev_should_fail()){errno=EIO;return -1;} if(g_bdev_sock>=0) return bsock_pwrite(buf,len,off); return pwrite(g_bdev_fd, buf, len, off); }
/* ÚJRAPRÓBÁLÓ bdev-I/O: a valós USB-pendrive ÁTMENETI hibáit (amit a block.so néha felszínre hoz)
 * elnyeli a FS-logika ELŐTT — különben az ext4/jbd2 hibakezelő-útja SIGTRAP/segfaultot ad. 8 próba
 * kis backoffal; csak a TARTÓS hiba jut át (EIO), amit a FS gracefully kezel (cp EIO, nem crash). */
static ssize_t bdev_pread(void *buf, size_t len, off_t off)
{ for (int t = 0; t < 8; t++) { ssize_t r = bdev_pread1(buf, len, off); if (r == (ssize_t)len) return r; usleep(2000); } return -1; }
static ssize_t bdev_pwrite(const void *buf, size_t len, off_t off)
{ for (int t = 0; t < 8; t++) { ssize_t r = bdev_pwrite1(buf, len, off); if (r == (ssize_t)len) return r; usleep(2000); } return -1; }

/* ===== buffer_head ===== */
/* ===== globális buffer-registry: a HELD (nem brelse-elt) dirty buffereket — pl. az ext4
 * group-descriptor + superblock, amelyeket a sbi->s_group_desc[] tart — a sync_blockdev
 * kiírja. Enélkül a szabad-blokk/inode-számlálók nem perzisztálnak (fsck "count wrong"). */
struct uk_bhnode { struct buffer_head *bh; struct uk_bhnode *next; };
static struct uk_bhnode *g_bhlist;
/* block-device mapping-kontextus: a jbd2 a metaadat-bufferek b_folio->mapping->host->i_sb-jét
 * dereferálja (jbd2_journal_cancel_revoke). Közös bdev-mapping, de PER-BUFFER folio kell,
 * mert a jbd2 a b_folio-ból olvassa a metaadatot (folio_set_bh/kmap_local_folio). */
static struct super_block g_bdev_sb2;
static struct inode g_bdev_inode2;
static struct address_space g_bdev_amap2;
static struct address_space *uk_bdev_mapping(void)
{
	if (!g_bdev_amap2.host) {
		g_bdev_inode2.i_sb = &g_bdev_sb2;
		g_bdev_amap2.host = &g_bdev_inode2;
	}
	return &g_bdev_amap2;
}
/* a buffer adatát (b_data) folio-szerűen elérhetővé tesszük: a folio _priv-je a b_data
 * LAPBÁZISA, így offset_in_folio(folio,b_data)= b_data&4095 és folio_address+offset == b_data. */
static void bh_attach_folio(struct buffer_head *bh)
{
	if (bh->b_folio || !bh->b_data) return;
	struct folio *fo = calloc(1, sizeof(*fo));
	if (!fo) return;
	fo->_priv = (void *)((unsigned long)bh->b_data & ~(unsigned long)(PAGE_SIZE - 1));
	fo->mapping = uk_bdev_mapping();
	bh->b_folio = fo;
}

/* virt_to_folio: a `x` címet tartalmazó folio-leíró (folio_address(fo) == a lap bázisa). A jbd2 a
 * fagyasztott-adat/escape ágon hívja (virt_to_folio(b_frozen_data)) — eddig NULL-stub volt → crash.
 * Megj.: per-hívás új folio-t alloc-ol (a journal new_bh-t a free_buffer_head NEM szabadítja) →
 * kis szivárgás a (rövid életű, per-parancs) processzben, cserébe HELYES journal-checksum. */
struct folio *virt_to_folio(const void *x)
{
	if (!x) return 0;
	struct folio *fo = calloc(1, sizeof(*fo));
	if (!fo) return 0;
	fo->_priv = (void *)((unsigned long)x & ~(unsigned long)(PAGE_SIZE - 1));
	fo->mapping = uk_bdev_mapping();
	return fo;
}
static void bh_register(struct buffer_head *bh)
{ bh_attach_folio(bh);
  struct uk_bhnode *n = malloc(sizeof(*n)); if (n) { n->bh = bh; n->next = g_bhlist; g_bhlist = n; } }
static void bh_writeback(struct buffer_head *bh);
static int bh_unregister(struct buffer_head *bh)
{ struct uk_bhnode **pp = &g_bhlist; for (struct uk_bhnode *n = g_bhlist; n; pp = &n->next, n = n->next) if (n->bh == bh) { *pp = n->next; free(n); return 1; } return 0; }
/* buffer-cache: a jbd2 a journal_head-et a CACHE-ELT bufferhez köti, és elvárja hogy
 * ugyanarra a blokkra ugyanazt a buffer_head-et kapja vissza (különben a metaadat-frissítés
 * — pl. az inode-tábla — sosem perzisztál). Ezért a getblk-család a meglévőt adja vissza. */
static struct buffer_head *bh_cache_find(sector_t block, unsigned size)
{ for (struct uk_bhnode *n = g_bhlist; n; n = n->next) if (n->bh->b_blocknr == block && n->bh->b_size == size) return n->bh; return 0; }
/* közös release: a regisztrált (cache-elt) buffert NEM szabadítjuk — a cache-ben marad,
 * hogy a következő getblk ugyanazt adja (jbd2-követelmény). Csak visszaírunk + refcount--.
 * A jbd2 journal-temp bh-ja NEM regisztrált — azt a free_buffer_head zárja. */
static void bh_release(struct buffer_head *bh)
{
	if (!bh) return;
	bh_writeback(bh);
	if (bh->b_count > 0) bh->b_count--;
}

/* közös olvasó-buffer EXPLICIT mérettel: bs bájtos buffert ad a `block`-ra (cache-elve). */
static struct buffer_head *uk_bread_n(sector_t block, unsigned bs)
{
	struct buffer_head *bh = bh_cache_find(block, bs);
	if (bh) {                                  /* cache-találat: ugyanaz a buffer (jbd2) */
		if (!(bh->b_state & (1UL << BH_Uptodate))) {
			if (bdev_pread(bh->b_data, bs, (off_t)block * bs) == (ssize_t)bs)
				bh->b_state |= (1UL << BH_Uptodate) | (1UL << BH_Mapped);
		}
		bh->b_count++;
		return bh;
	}
	bh = calloc(1, sizeof(*bh));
	if (!bh) return NULL;
	bh->b_data = malloc(bs);
	bh->b_size = bs;
	bh->b_blocknr = block;
	if (!bh->b_data) { free(bh); return NULL; }
	if (bdev_pread(bh->b_data, bs, (off_t)block * bs) != (ssize_t)bs) {
		free(bh->b_data); free(bh); return NULL;
	}
	bh->b_state = (1UL << BH_Uptodate) | (1UL << BH_Mapped);
	bh->b_count = 1;
	bh_register(bh);
	return bh;
}
struct buffer_head *sb_bread(struct super_block *sb, sector_t block)
{ return uk_bread_n(block, sb->s_blocksize ? sb->s_blocksize : g_blocksize); }
struct buffer_head *sb_getblk(struct super_block *sb, sector_t block)
{
	unsigned bs = sb->s_blocksize ? sb->s_blocksize : g_blocksize;
	struct buffer_head *bh = bh_cache_find(block, bs);
	if (bh) { bh->b_count++; return bh; }
	bh = calloc(1, sizeof(*bh));
	if (!bh) return NULL;
	bh->b_data = calloc(1, bs); bh->b_size = bs; bh->b_blocknr = block; bh->b_count = 1;
	bh->b_state = (1UL << BH_Mapped);
	bh_register(bh);
	return bh;
}
/* sb_getblk_gfp (ext4): a backendről OLVASOTT, uptodate buffert ad — így az ext4
 * __ext4_sb_bread_gfp `ext4_buffer_uptodate` ága igaz lesz, és átugorja a bio-olvasást. */
struct buffer_head *sb_getblk_gfp(struct super_block *sb, sector_t block, gfp_t gfp)
{ (void)gfp; return sb_bread(sb, block); }
/* __bread/__getblk: az ntfs3 (és mások) az ELSŐ argnak a block_device-t adják (NEM sb!), a
 * blokkméretet a `size`-ban — ezért a `size`-ot KELL használni, NEM az sb->s_blocksize-ot.
 * (A régi `(void)size; sb_bread(sb,block)` a bdev-et sb-ként értelmezte → s_blocksize=garbage(0)
 * → g_blocksize=512 buffer, de az ntfs3 4096-ot ír → HEAP-TÚLÍRÁS, pl. ntfs_sb_write_run-ban.) */
struct buffer_head *__bread(struct super_block *sb_or_bdev, sector_t block, unsigned size)
{ (void)sb_or_bdev; return uk_bread_n(block, size ? size : g_blocksize); }
struct buffer_head *sb_find_get_block(struct super_block *sb, sector_t block)
{ (void)sb; (void)block; return NULL; }   /* nincs cache: mindig friss olvasás */
void sb_breadahead(struct super_block *sb, sector_t block) { (void)sb; (void)block; }

static void bh_writeback(struct buffer_head *bh)
{
	if (bdev_active() && (bh->b_state & (1UL << BH_Dirty)))
		bdev_pwrite(bh->b_data, bh->b_size, (off_t)bh->b_blocknr * bh->b_size);
	bh->b_state &= ~(1UL << BH_Dirty);
}
void mark_buffer_dirty(struct buffer_head *bh) { bh->b_state |= (1UL << BH_Dirty); }
void mark_buffer_dirty_inode(struct buffer_head *bh, void *inode) { (void)inode; mark_buffer_dirty(bh); }
int  sync_dirty_buffer(struct buffer_head *bh) { bh_writeback(bh); return 0; }
int  write_dirty_buffer(struct buffer_head *bh, int op) { (void)op; bh_writeback(bh); return 0; }
/* brelse: visszaír, majd REFCOUNT (b_count) alapján szabadít — csak 0-nál.
 * A fat duplikált bh-kat tárol a bhs[]-ben és get_bh-val növeli a számlálót; enélkül
 * a mindig-szabadító brelse double-free-t (vagy b_data=NULL use-after-free-t) okozna. */
void brelse(struct buffer_head *bh) { bh_release(bh); }
void __brelse(struct buffer_head *bh) { bh_release(bh); }
/* put_bh: az ntfs3 ezzel (nb_put-on át) engedi el a piszkos MFT-rekord/INDX-blokk
 * buffereit — brelse-szel azonos szemantika kell (writeback + refcount-szabadítás),
 * különben a könyvtár-index módosítása sosem kerül vissza az image-re. */
void put_bh(struct buffer_head *bh) { bh_release(bh); }
void bforget(struct buffer_head *bh) { if (!bh) return; if (--bh->b_count <= 0 && bh_unregister(bh)) { if (bh->b_folio) free(bh->b_folio); free(bh->b_data); free(bh); } }
void wait_on_buffer(struct buffer_head *bh) { (void)bh; }   /* szinkron I/O — nincs várakozás */
void lock_buffer(struct buffer_head *bh) { if (bh) bh->b_state |= (1UL << BH_Locked); }
/* trylock_buffer: a jbd2-checkpoint (jbd2_log_do_checkpoint) ezzel próbálja zárolni a buffert;
 * a no-op-0 stub miatt MINDIG sikertelen volt → a checkpoint-loop örökre pörgött (hang). */
int trylock_buffer(struct buffer_head *bh)
{ if (!bh) return 0; if (bh->b_state & (1UL << BH_Locked)) return 0; bh->b_state |= (1UL << BH_Locked); return 1; }
void unlock_buffer(struct buffer_head *bh) { if (bh) bh->b_state &= ~(1UL << BH_Locked); }
/* submit_bh: VALÓDI szinkron blokk-I/O (ext4 ezen olvas/ír metaadatot). A REQ_OP a 0-2.
 * bitekben (READ=0, WRITE=1); a REQ_SYNC a 3. bit (blk_types.h), ezért a maszk 0x7 — NEM
 * 0xff! (Különben WRITE|SYNC|FUA=0x209 → op=9 → tévesen READ → a SB-írás OLVASÁS lett!) */
int submit_bh(blk_opf_t opf, struct buffer_head *bh)
{
	if (!bh) return -22;
	int op = (int)(opf & 0x7);             /* REQ_OP (READ=0/WRITE=1), a SYNC/FUA fölötte */
	off_t off = (off_t)bh->b_blocknr * bh->b_size;
	int ok = 1;
	if (op == 1 /*REQ_OP_WRITE*/) {
		if (bdev_active() && bdev_pwrite(bh->b_data, bh->b_size, off) != (ssize_t)bh->b_size) ok = 0;
		else bh->b_state &= ~(1UL << BH_Dirty);
	} else {                               /* REQ_OP_READ */
		if (bdev_active() && bdev_pread(bh->b_data, bh->b_size, off) == (ssize_t)bh->b_size)
			bh->b_state |= (1UL << BH_Uptodate);
		else ok = 0;
	}
	bh->b_state &= ~(1UL << BH_Locked);
	if (bh->b_end_io) bh->b_end_io(bh, ok);
	return 0;
}
void ll_rw_block(int op, int flags, int nr, struct buffer_head *bhs[])
{ (void)flags; for (int i = 0; i < nr; i++) if (bhs[i]) submit_bh((blk_opf_t)op, bhs[i]); }
/* end_buffer_read/write_sync: a submit_bh b_end_io-ja — uptodate + unlock + put_bh (a get_bh párja) */
void end_buffer_read_sync(struct buffer_head *bh, int uptodate)
{ if (!bh) return; if (uptodate) bh->b_state |= (1UL<<BH_Uptodate); else bh->b_state &= ~(1UL<<BH_Uptodate); bh->b_state &= ~(1UL<<BH_Locked); put_bh(bh); }
void end_buffer_write_sync(struct buffer_head *bh, int uptodate)
{ if (!bh) return; if (!uptodate) bh->b_state |= (1UL<<BH_Write_EIO); bh->b_state &= ~(1UL<<BH_Locked); put_bh(bh); }
/* bh_uptodate_or_lock: ha uptodate → 1 (nincs olvasás); különben lock + 0 (olvasandó) */
int bh_uptodate_or_lock(struct buffer_head *bh)
{ if (!bh) return 0; if (bh->b_state & (1UL<<BH_Uptodate)) return 1; bh->b_state |= (1UL<<BH_Locked); return 0; }
/* bh_read_nowait / bh_readahead_batch: szinkron háttér — egyszerűen olvasunk submit_bh-val */
int bh_read_nowait(struct buffer_head *bh, blk_opf_t op_flags)
{ if (!bh) return -22; if (bh->b_state & (1UL<<BH_Uptodate)) return 1; lock_buffer(bh); get_bh(bh); bh->b_end_io = end_buffer_read_sync; submit_bh(0 | op_flags, bh); return 0; }
void bh_readahead_batch(int nr, struct buffer_head *bhs[], blk_opf_t op_flags)
{ for (int i = 0; i < nr; i++) if (bhs[i] && !(bhs[i]->b_state & (1UL<<BH_Uptodate))) bh_read_nowait(bhs[i], op_flags); }

/* ===== inode-életciklus (egyszerű hash a fat_iget-hez) ===== */
static unsigned long g_ino_next = 1;
#define IHASH 1024
static struct inode *g_ihash[IHASH];   /* láncolás i_hash.next-en át */

static struct inode *alloc_vfs_inode(struct super_block *sb)
{
	struct inode *inode = sb->s_op && sb->s_op->alloc_inode ? sb->s_op->alloc_inode(sb) : calloc(1, sizeof(struct inode));
	if (!inode) return NULL;
	inode->i_sb = sb;
	inode->i_count.counter = 1;
	inode->i_mapping = &inode->i_data;
	inode->i_data.host = inode;
	inode->i_blkbits = sb->s_blocksize_bits;
	return inode;
}
void *alloc_inode_sb(struct super_block *sb, struct kmem_cache *cache, gfp_t gfp)
{ (void)sb; return kmem_cache_zalloc(cache, gfp); }

struct inode *new_inode(struct super_block *sb)
{
	struct inode *inode = alloc_vfs_inode(sb);
	if (inode) { inode->i_ino = ++g_ino_next; inode->__i_nlink = 1; }  /* inode_init_always: nlink=1 */
	return inode;
}
struct inode *iget_locked(struct super_block *sb, unsigned long ino)
{
	struct inode *i = g_ihash[ino % IHASH];
	for (; i; i = i->i_hash.next ? (struct inode *)i->i_hash.next : NULL)
		if (i->i_ino == ino && i->i_sb == sb) { i->i_count.counter++; return i; }
	i = alloc_vfs_inode(sb);
	if (!i) return NULL;
	i->i_ino = ino; i->i_state = I_NEW;
	i->i_hash.next = (struct hlist_node *)g_ihash[ino % IHASH];
	g_ihash[ino % IHASH] = i;
	return i;
}
struct inode *iget5_locked(struct super_block *sb, unsigned long hashval,
	int (*test)(struct inode *, void *), int (*set)(struct inode *, void *), void *data)
{
	struct inode *i;
	for (i = g_ihash[hashval % IHASH]; i; i = (struct inode *)i->i_hash.next)
		if (i->i_sb == sb && test && test(i, data)) { i->i_count.counter++; return i; }
	i = alloc_vfs_inode(sb);
	if (!i) return NULL;
	i->i_state = I_NEW; if (set) set(i, data);
	i->i_hash.next = (struct hlist_node *)g_ihash[hashval % IHASH];
	g_ihash[hashval % IHASH] = i;
	return i;
}
struct inode *ilookup(struct super_block *sb, unsigned long ino)
{
	struct inode *i;
	for (i = g_ihash[ino % IHASH]; i; i = (struct inode *)i->i_hash.next)
		if (i->i_ino == ino && i->i_sb == sb) { i->i_count.counter++; return i; }
	return NULL;
}
void unlock_new_inode(struct inode *inode) { if (inode) inode->i_state &= ~I_NEW; }
void iget_failed(struct inode *inode) { if (inode) { inode->i_state |= (1<<5); } }
void iput(struct inode *inode) { if (inode && --inode->i_count.counter <= 0) { /* megtartjuk a hashben */ } }
struct inode *igrab(struct inode *inode) { if (inode) inode->i_count.counter++; return inode; }
void ihold(struct inode *inode) { if (inode) inode->i_count.counter++; }
int  insert_inode_locked(struct inode *inode) { (void)inode; return 0; }
void insert_inode_hash(struct inode *inode)
{ unsigned long h = inode->i_ino % IHASH; inode->i_hash.next = (struct hlist_node *)g_ihash[h]; g_ihash[h] = inode; }
/* Unhash an evicted inode so a future iget re-reads it fresh from disk. CRITICAL:
 * FAT reuses a deleted entry's directory slot (-> same i_pos/i_ino) for the next
 * file/dir created there. With the inode left cached, iget_locked() would return
 * the STALE inode of the deleted file (wrong mode/clusters) — so a new directory
 * at a reused slot fails the is-a-dir check in api_walk, and lookups/creates
 * under it flakily fail with ENOENT/EIO. Scan all buckets (iget_locked keys on
 * i_ino, iget5_locked on a driver hashval) and remove by pointer; don't free
 * (stale leaked dentries may still point at it — unhashing is enough to stop new
 * lookups from resurrecting it). */
void remove_inode_hash(struct inode *inode)
{
	if (!inode) return;
	for (int h = 0; h < IHASH; h++) {
		struct inode *cur = g_ihash[h], *prev = 0;
		while (cur) {
			struct inode *nx = cur->i_hash.next ? (struct inode *)cur->i_hash.next : 0;
			if (cur == inode) {
				if (prev) prev->i_hash.next = cur->i_hash.next;
				else g_ihash[h] = nx;
				cur->i_hash.next = 0;
				return;
			}
			prev = cur; cur = nx;
		}
	}
}
void clear_inode(struct inode *inode) { (void)inode; }
/* bad-inode JELZŐ (i_state bit), NEM i_ino==0 — a $MFT legitimen i_ino=0 (MFT_REC_MFT)! */
#define UK_I_BAD (1<<13)
void make_bad_inode(struct inode *inode) { if (inode) inode->i_state |= UK_I_BAD; }
int  is_bad_inode(struct inode *inode) { return inode && (inode->i_state & UK_I_BAD); }
void inode_init_once(struct inode *inode) { (void)inode; }
unsigned int iunique(struct super_block *sb, int reserved) { (void)sb; (void)reserved; return ++g_ino_next; }
/* __mark_inode_dirty: a kernelhez hűen a driver dirty_inode-opját hívja (ext4: ext4_dirty_inode
 * → ext4_mark_inode_dirty → ext4_do_update_inode), ami a bufferbe írja az inode-ot (i_size!).
 * Az inline-írás write_inline_data_end-je CSAK ezzel jelzi az i_size-frissítést (nem
 * ext4_mark_inode_dirty-vel, mint a blokkos út) — a no-op miatt az i_size 0 maradt a lemezen. */
void __mark_inode_dirty(struct inode *inode, int flags)
{
	if (inode && inode->i_sb && inode->i_sb->s_op && inode->i_sb->s_op->dirty_inode)
		inode->i_sb->s_op->dirty_inode(inode, flags);
}
void mark_inode_dirty(struct inode *inode) { __mark_inode_dirty(inode, I_DIRTY_SYNC | I_DIRTY_DATASYNC); }
void inode_lock(struct inode *i) { (void)i; }
void inode_unlock(struct inode *i) { (void)i; }
void inode_lock_shared(struct inode *i) { (void)i; }
void inode_unlock_shared(struct inode *i) { (void)i; }
void inode_dio_wait(struct inode *i) { (void)i; }
int  inode_needs_sync(struct inode *i) { (void)i; return 0; }
/* Real wall-clock time, so files created/modified through the engine get correct
 * timestamps (FAT/exFAT encode this into the directory entry). Previously these
 * returned epoch 0, which showed up as "Jan 1 1970" (in-memory inode) or "Jan 1
 * 1980" (FAT clamps 0 to its 1980 minimum) in ls. */
struct timespec64 current_time(struct inode *inode)
{
	(void)inode;
	struct timespec ts; struct timespec64 t = { 0, 0 };
	if (clock_gettime(CLOCK_REALTIME, &ts) == 0) { t.tv_sec = ts.tv_sec; t.tv_nsec = ts.tv_nsec; }
	return t;
}
struct timespec64 inode_set_ctime_current(struct inode *inode)
{ struct timespec64 t = current_time(inode); if (inode) inode->__i_ctime = t; return t; }

/* ===== dentry ===== */
static struct dentry *alloc_dentry(struct super_block *sb, struct inode *inode)
{
	struct dentry *d = calloc(1, sizeof(*d));
	if (!d) return NULL;
	d->d_sb = sb; d->d_inode = inode;
	d->d_parent = d;
	return d;
}
struct dentry *d_make_root(struct inode *root_inode)
{
	if (!root_inode) return NULL;
	struct dentry *d = alloc_dentry(root_inode->i_sb, root_inode);
	if (d) { d->d_name.name = (const unsigned char *)"/"; d->d_name.len = 1; }
	return d;
}
void d_instantiate(struct dentry *dentry, struct inode *inode) { if (dentry) dentry->d_inode = inode; }
/* d_instantiate_new (ext4 create): beállítja a d_inode-ot + törli az I_NEW-t (unlock_new_inode) */
void d_instantiate_new(struct dentry *dentry, struct inode *inode)
{ if (dentry) dentry->d_inode = inode; if (inode) inode->i_state &= ~I_NEW; }
struct dentry *d_splice_alias(struct inode *inode, struct dentry *dentry)
{
	if (IS_ERR(inode)) return (struct dentry *)inode;  /* hibakód: NE állítsuk d_inode-ba */
	if (dentry) dentry->d_inode = inode;               /* lehet NULL (nincs ilyen név) */
	return NULL;
}
struct dentry *d_obtain_alias(struct inode *inode)
{ return inode ? alloc_dentry(inode->i_sb, inode) : NULL; }
struct dentry *d_find_alias(struct inode *inode) { (void)inode; return NULL; }
void d_add(struct dentry *dentry, struct inode *inode) { if (dentry) dentry->d_inode = inode; }
void d_move(struct dentry *a, struct dentry *b) { (void)a; (void)b; }
void dput(struct dentry *dentry) { (void)dentry; }
struct dentry *dget(struct dentry *dentry) { return dentry; }
void set_default_d_op(struct super_block *sb, const struct dentry_operations *ops) { sb->s_d_op = (void *)ops; }

/* ===== mount-út: register_filesystem + get_tree_bdev + uk_mount ===== */
static struct file_system_type *g_fs_list;
int register_filesystem(struct file_system_type *fs)
{ fs->next = g_fs_list; g_fs_list = fs; if (getenv("UK_FS_DEBUG")) fprintf(stderr, "[ukfs] register_filesystem: %s\n", fs->name ? fs->name : "?"); return 0; }
int unregister_filesystem(struct file_system_type *fs) { (void)fs; return 0; }
void kill_block_super(struct super_block *sb) { (void)sb; }

/* a get_tree_bdev: a fill_super-t hívja a már felépített super_block-kal */
static struct super_block *g_mount_sb;     /* az aktuális mount sb-je */
int get_tree_bdev(struct fs_context *fc,
		  int (*fill_super)(struct super_block *sb, struct fs_context *fc))
{
	struct super_block *sb = g_mount_sb;
	/* exfat/ext4 az init_fs_context-ben allokálja az s_fs_info-t és fc-be teszi;
	 * át kell adni a sb-nek a fill_super előtt (a vfat ezt magában a fill_super-ben teszi). */
	if (fc->s_fs_info) sb->s_fs_info = fc->s_fs_info;
	int err = fill_super(sb, fc);
	if (err) { if(getenv("UK_FS_DEBUG")) fprintf(stderr, "[ukfs] fill_super hiba: %d\n", err); return err; }
	fc->root = sb->s_root;
	return 0;
}
int get_tree_nodev(struct fs_context *fc,
		   int (*fill_super)(struct super_block *sb, struct fs_context *fc))
{ return get_tree_bdev(fc, fill_super); }

/* fájl-olvasás: a fat aops->bmap-jével a fájl-blokkot fizikai blokkra képezzük,
 * majd sb_bread-del (backend) olvassuk — ez a `cat` adat-útja. */
ssize_t uk_read(struct dentry *dentry, char *buf, size_t len, loff_t pos)
{
	struct inode *inode = dentry->d_inode;
	struct address_space *m = inode->i_mapping;
	unsigned bs = inode->i_sb->s_blocksize ? inode->i_sb->s_blocksize : g_blocksize;
	if (pos >= inode->i_size) return 0;
	if ((loff_t)(pos + len) > inode->i_size) len = inode->i_size - pos;
	size_t done = 0;
	/* gyors út (vfat/exfat/ext4/ntfs3): a_ops->bmap -> fizikai blokk -> sb_bread */
	if (m && m->a_ops && m->a_ops->bmap) {
		int resident = 0;
		while (done < len) {
			sector_t fblock = (pos + done) / bs;
			unsigned boff = (pos + done) % bs;
			sector_t phys = m->a_ops->bmap(m, fblock);
			unsigned chunk = bs - boff; if (chunk > len - done) chunk = len - done;
			if (!phys) {
				/* phys==0: LYUK (sparse fájl) VAGY rezidens adat (ntfs3 kis fájl, nincs blokk).
				 * Megkülönböztetés i_blocks-szal: ha a fájlnak VAN allokált blokkja, ez egy LYUK
				 * -> nullák, megyünk tovább (a sparse fájl adat-extentjeit így is elérjük); ha
				 * NINCS (i_blocks==0), lehet rezidens -> az egész olvasást a read_folio-fallbackra. */
				if (inode->i_blocks > 0) { memset(buf + done, 0, chunk); done += chunk; continue; }
				resident = 1; break;
			}
			struct buffer_head *bh = sb_bread(inode->i_sb, phys);
			if (!bh) break;
			memcpy(buf + done, bh->b_data + boff, chunk);
			brelse(bh);
			done += chunk;
		}
		if (done >= len) return done;
		if (!resident && done > 0) return done;   /* nem rezidens (sparse): a kapott rész kész */
	}
	/* fallback (ntfs3 rezidens/nem-rezidens): a driver VALÓDI a_ops->read_folio-ja, lapról
	 * lapra — ez az iomap_read_folio-n át kezeli a rezidens (MFT-be ágyazott) adatot is,
	 * amit a bmap nem tud. Pontosan az, amit a kernel page-cache-e tenne, csak cache nélkül. */
	if (m && m->a_ops && m->a_ops->read_folio) {
		struct file f; memset(&f, 0, sizeof(f));
		f.f_inode = inode; f.f_mapping = m; f.f_op = inode->i_fop; f.f_path.dentry = dentry;
		while (done < len) {
			loff_t fp = pos + done;
			pgoff_t idx = fp / 4096;
			unsigned boff = fp % 4096;
			struct folio *fo = calloc(1, sizeof(*fo));
			if (!fo) break;
			fo->_priv = calloc(1, 4096);
			if (!fo->_priv) { free(fo); break; }
			fo->mapping = m; fo->index = idx;
			fo->page.mapping = m; fo->page.index = idx; fo->page._priv = fo->_priv;
			fo->flags |= 1UL;   /* PG_locked — a read_folio-kontraktus: a hívó zárol, a driver
			                       felold (ext4_readpage_inline BUG_ON(!folio_test_locked)). */
			int e = m->a_ops->read_folio(&f, fo);
			unsigned chunk = 4096 - boff; if (chunk > len - done) chunk = len - done;
			if (!e) memcpy(buf + done, (char *)fo->_priv + boff, chunk);
			free(fo->_priv); free(fo);
			if (e) break;
			done += chunk;
		}
	}
	return done;
}

/* a host-szintű belépő: mountolja <fstype>-ot a <devpath>-ról, visszaadja a root dentryt */
struct dentry *uk_mount(const char *fstype, const char *devpath)
{
	struct file_system_type *fs;
	for (fs = g_fs_list; fs; fs = fs->next) if (!strcmp(fs->name, fstype)) break;
	if (!fs) { fprintf(stderr, "[ukfs] ismeretlen fs: %s\n", fstype); return NULL; }

	if (devpath[0] == '@') {            /* block-over-socket: "@io.neoterm.block" */
		if (bsock_open(devpath + 1) != 0) { fprintf(stderr, "[ukfs] block-socket nem elérhető: %s\n", devpath + 1); return NULL; }
		g_bdev_sock = 0;
	} else {
		g_bdev_fd = open(devpath, O_RDWR);
		if (g_bdev_fd < 0) { const char *ll=getenv("UK_LOGLEVEL"); if(!ll||atoi(ll)>=6) fprintf(stderr, "[ukfs] eszköz nem nyitható: %s\n", devpath); return NULL; }
	}

	static struct block_device bdev;
	static struct super_block sb;
	memset(&sb, 0, sizeof(sb));
	static struct backing_dev_info bdi = { .ra_pages = 32, .io_pages = 32 };
	sb.s_bdev = &bdev; sb.s_type = fs; sb.s_bdi = &bdi;
	sb.s_inodes.next = sb.s_inodes.prev = &sb.s_inodes;
	g_mount_sb = &sb;

	static struct fs_context fc;
	memset(&fc, 0, sizeof(fc));
	fc.fs_type = fs; fc.purpose = FS_CONTEXT_FOR_MOUNT; fc.source = devpath;
	if (fs->init_fs_context && fs->init_fs_context(&fc)) { fprintf(stderr, "[ukfs] init_fs_context hiba\n"); return NULL; }
	/* az ntfs3 a friss mkfs.ntfs-image VOLUME_FLAG_DIRTY-jét force nélkül csak RO-ban mountolja;
	 * RW-hez a valódi "force" mount-opciót injektáljuk a fs_context parse_param-ján át. */
	if (!strcmp(fstype, "ntfs3") && fc.ops && fc.ops->parse_param) {
		struct fs_parameter param; memset(&param, 0, sizeof(param));
		param.key = "force";
		fc.ops->parse_param(&fc, &param);
	}
	/* ext4: a delalloc (alapból BE) az ext4_da_aops-ot adja (bonyolult writeback-mapper).
	 * "nodelalloc"-kal az egyszerű ext4_aops (write_begin/write_end) megy, amit a meglévő
	 * ukfs_write_file-ág kezel (mint vfat/exfat). */
	if (!strcmp(fstype, "ext4") && fc.ops && fc.ops->parse_param) {
		struct fs_parameter param; memset(&param, 0, sizeof(param));
		param.key = "nodelalloc"; param.type = fs_value_is_flag;
		fc.ops->parse_param(&fc, &param);
	}
	if (!fc.ops || !fc.ops->get_tree) { fprintf(stderr, "[ukfs] nincs get_tree\n"); return NULL; }
	if (fc.ops->get_tree(&fc)) return NULL;
	g_api_root = fc.root;
	if(getenv("UK_FS_DEBUG")) fprintf(stderr, "[ukfs] mount OK: %s a %s-ról (root=%p)\n", fstype, devpath, (void *)fc.root);
	return fc.root;
}

/* ===== nls (kódlap) — latin1/cp437-szerű minimál implementáció a vfat-hoz ===== */
static int nls_uni2char(wchar_t uni, unsigned char *out, int boundlen)
{ if (boundlen < 1) return -7; out[0] = (uni < 0x100) ? (unsigned char)uni : '?'; return 1; }
static int nls_char2uni(const unsigned char *rawstring, int boundlen, wchar_t *uni)
{ if (boundlen < 1) return -7; *uni = rawstring[0]; return 1; }
static unsigned char g_lower[256], g_upper[256];
static struct nls_table g_nls = {
	.charset = "iso8859-1",
	.uni2char = nls_uni2char, .char2uni = nls_char2uni,
	.charset2lower = g_lower, .charset2upper = g_upper,
};
__attribute__((constructor)) static void nls_init_tables(void)
{ for (int c = 0; c < 256; c++) { g_lower[c] = (c >= 'A' && c <= 'Z') ? c + 32 : c; g_upper[c] = (c >= 'a' && c <= 'z') ? c - 32 : c; } }
struct nls_table *load_nls(const char *charset) { (void)charset; return &g_nls; }
struct nls_table *load_nls_default(void) { return &g_nls; }
void unload_nls(struct nls_table *t) { (void)t; }

int utf16s_to_utf8s(const wchar_t *pwcs, int len, enum utf16_endian endian, u8 *s, int maxlen)
{
	(void)endian; int o = 0;
	for (int i = 0; i < len && o < maxlen; i++) { unsigned c = pwcs[i]; if (!c) break; if (c < 0x80) s[o++] = c; else if (o+1 < maxlen) { s[o++] = 0xc0|(c>>6); s[o++] = 0x80|(c&0x3f); } }
	return o;
}
int utf8s_to_utf16s(const u8 *s, int len, enum utf16_endian endian, wchar_t *pwcs, int maxlen)
{
	(void)endian; int o = 0, i = 0;
	while (i < len && o < maxlen) { unsigned c = s[i++]; if (c >= 0xc0 && i < len) { c = ((c&0x1f)<<6) | (s[i++]&0x3f); } pwcs[o++] = c; }
	return o;
}

/* ===== név-hash ===== */
unsigned long full_name_hash(const void *salt, const char *name, unsigned int len)
{ (void)salt; unsigned long h = 0; while (len--) h = h*31 + (unsigned char)*name++; return h; }
unsigned int init_name_hash(const void *salt) { (void)salt; return 0; }
unsigned long partial_name_hash(unsigned long c, unsigned long prev) { return prev*31 + c; }
unsigned int end_name_hash(unsigned long hash) { return (unsigned int)hash; }

/* ===== apró segédek + no-op stubok ===== */
int current_umask(void) { return 0; }
struct timezone *get_sys_tz(void) { return &sys_tz; }

bool dir_emit_dot(struct file *file, struct dir_context *ctx)
{ (void)file; return ctx->actor(ctx, ".", 1, ctx->pos, 1, DT_DIR) == 0; }
bool dir_emit_dotdot(struct file *file, struct dir_context *ctx)
{ (void)file; return ctx->actor(ctx, "..", 2, ctx->pos, 1, DT_DIR) == 0; }

/* statfs/attr/egyéb — a minimál read-úthoz no-op */
int setattr_prepare(struct mnt_idmap *i, struct dentry *d, struct iattr *a) { (void)i;(void)d;(void)a; return 0; }
/* setattr_copy: a VALÓDI VFS-szemantika — az iattr-mezőket az inode-ba másolja (mode/uid/gid/
 * idők). A no-op miatt a chmod/chown nem frissítette az i_mode/i_uid/i_gid-et. */
void setattr_copy(struct mnt_idmap *idmap, struct inode *in, const struct iattr *a)
{
	(void)idmap;
	unsigned int v = a->ia_valid;
	if (v & ATTR_UID)   in->i_uid = a->ia_uid;
	if (v & ATTR_GID)   in->i_gid = a->ia_gid;
	if (v & ATTR_ATIME) in->i_atime = a->ia_atime;
	if (v & ATTR_MTIME) in->i_mtime = a->ia_mtime;
	if (v & ATTR_CTIME) in->__i_ctime = a->ia_ctime;
	if (v & ATTR_MODE)  in->i_mode = a->ia_mode;
}
void generic_fillattr(struct mnt_idmap *i, u32 m, struct inode *in, struct kstat *st)
{ (void)i;(void)m; if (in && st) { st->mode = in->i_mode; st->ino = in->i_ino; st->size = in->i_size; st->blocks = in->i_blocks; } }
int security_inode_setattr(struct mnt_idmap *i, struct dentry *d, struct iattr *a) { (void)i;(void)d;(void)a; return 0; }
int sync_filesystem(struct super_block *sb) { (void)sb; return 0; }
void truncate_setsize(struct inode *inode, loff_t newsize) { if (inode) inode->i_size = newsize; }
void truncate_inode_pages(struct address_space *m, loff_t l) { (void)m;(void)l; }
void truncate_inode_pages_final(struct address_space *m) { (void)m; }
void truncate_pagecache(struct inode *inode, loff_t to) { (void)inode;(void)to; }
void super_set_uuid(struct super_block *sb, const u8 *uuid, unsigned len) { (void)sb;(void)uuid;(void)len; }

/* ===== linker-kielégítő stubok (a minimál read-úton nem hívódnak, vagy no-op) ===== */
#include <stdarg.h>
/* Route _printk through vprintk (uk_vformat) so kernel format extensions like
 * %pV (struct va_format, used by fat_fs_error/_fat_msg) are formatted correctly
 * and the message goes through loglevel filtering + the dmesg buffer. The raw
 * vfprintf here does NOT understand %pV, which printed FAT errors as a bare
 * pointer + literal 'V' ("FAT-fs (): 0x... V") and hid the real message. */
int _printk(const char *fmt, ...) { va_list a; va_start(a, fmt); int r = vprintk(fmt, a); va_end(a); return r; }
void panic(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); abort(); }
s64 div_s64_rem(s64 dividend, s32 divisor, s32 *remainder) { if (remainder) *remainder = dividend % divisor; return dividend / divisor; }
unsigned int inode_time_dirty_flag(struct inode *i, int f) { (void)i; (void)f; return 0; }
u64 huge_encode_dev(dev_t dev) { return (u64)dev; }
void call_rcu(struct rcu_head *head, void (*func)(struct rcu_head *)) { if (func) func(head); }
void rcu_barrier(void) {}

/* buffer_head/page-cache write+bmap út — a read-only readdirhez nem kell, stub */
int map_bh(struct buffer_head *bh, struct super_block *sb, sector_t block)
{ bh->b_blocknr = block; bh->b_size = sb->s_blocksize; bh->b_state |= (1UL << BH_Mapped); return 0; }
sector_t generic_block_bmap(struct address_space *m, sector_t block, get_block_t *get_block)
{
	struct buffer_head tmp; memset(&tmp, 0, sizeof(tmp));
	struct inode *inode = m->host;
	tmp.b_size = inode->i_sb->s_blocksize;
	get_block(inode, block, &tmp, 0);   /* fat_get_block_bmap kitölti b_blocknr-t */
	return tmp.b_blocknr;
}
void mpage_readahead(struct readahead_control *rac, get_block_t gb) { (void)rac;(void)gb; }
int  mpage_read_folio(struct folio *folio, get_block_t gb) { (void)folio;(void)gb; return 0; }
int  __mpage_writepages(struct address_space *m, struct writeback_control *w, get_block_t gb, void *d) { (void)m;(void)w;(void)gb;(void)d; return 0; }
ssize_t blockdev_direct_IO(struct kiocb *k, struct inode *i, struct iov_iter *it, get_block_t *gb) { (void)k;(void)i;(void)it;(void)gb; return 0; }
int block_write_begin(struct address_space *m, loff_t pos, unsigned len, struct folio **f, get_block_t *gb);
/* cont_write_begin: a FAT ezt hívja (folytonos fájlok); a mi block_write_begin-ünkre vezetjük */
int  cont_write_begin(const struct kiocb *k, struct address_space *m, loff_t pos, unsigned len, struct folio **f, void **fs, get_block_t *gb, loff_t *b) { (void)k;(void)fs;(void)b; return block_write_begin(m, pos, len, f, gb); }
struct uk_wfolio;
int generic_write_end(const struct kiocb *k, struct address_space *m, loff_t pos, unsigned len, unsigned copied, struct folio *f, void *fs)
{
	(void)k; (void)len; (void)fs;
	struct uk_wfolio { struct folio fo; char data[8192]; struct buffer_head bh[16]; int nbh; unsigned bs; struct inode *inode; pgoff_t index; } *w = (void *)f;
	for (int b = 0; b < w->nbh; b++) {
		struct buffer_head *bh = &w->bh[b];
		if (bh->b_blocknr)
			bdev_pwrite(bh->b_data, w->bs, (off_t)bh->b_blocknr * w->bs);  /* vissza az image-re */
	}
	struct inode *inode = m->host;
	if (pos + copied > inode->i_size) inode->i_size = pos + copied;
	free(w);
	return copied;
}
int  block_read_full_folio(struct folio *f, get_block_t *gb) { (void)f;(void)gb; return 0; }
int  block_write_full_folio(struct folio *f, struct writeback_control *w, void *gb) { (void)f;(void)w;(void)gb; return 0; }
bool block_dirty_folio(struct address_space *m, struct folio *f) { (void)m;(void)f; return true; }
void block_invalidate_folio(struct folio *f, size_t o, size_t l) { (void)f;(void)o;(void)l; }
int  block_truncate_page(struct address_space *m, loff_t from, get_block_t *gb) { (void)m;(void)from;(void)gb; return 0; }
int  generic_cont_expand_simple(struct inode *i, loff_t size) { (void)i;(void)size; return 0; }
int  buffer_migrate_folio(struct address_space *m, struct folio *d, struct folio *s, int mode) { (void)m;(void)d;(void)s;(void)mode; return 0; }

/* metadata-bh (mmb_*) — stub */
void mmb_init(struct mapping_metadata_bhs *m, struct address_space *a) { (void)m;(void)a; }
void mmb_mark_buffer_dirty(struct buffer_head *bh, struct mapping_metadata_bhs *m) { (void)m; mark_buffer_dirty(bh); }
int  mmb_sync(struct mapping_metadata_bhs *m) { (void)m; return 0; }
int  mmb_fsync_noflush(struct mapping_metadata_bhs *m) { (void)m; return 0; }
void mmb_invalidate(struct mapping_metadata_bhs *m) { (void)m; }

/* filemap/truncate/sync — read-only stub */
int  filemap_fdatawrite(struct address_space *m) { (void)m; return 0; }
int  filemap_fdatawrite_range(struct address_space *m, loff_t s, loff_t e) { (void)m;(void)s;(void)e; return 0; }
int  filemap_fdatawait_range(struct address_space *m, loff_t s, loff_t e) { (void)m;(void)s;(void)e; return 0; }
int  filemap_flush(struct address_space *m) { (void)m; return 0; }
int  sync_inode_metadata(struct inode *i, int wait) { (void)i;(void)wait; return 0; }
int  write_inode_now(struct inode *i, int sync) { (void)i;(void)sync; return 0; }
int  sync_blockdev_nowait(struct block_device *b) { (void)b; return 0; }
int  blkdev_issue_flush(struct block_device *b) { (void)b; return 0; }
void fsnotify_change(struct dentry *d, unsigned int m) { (void)d;(void)m; }

/* attr/idmap/fs_parser segédek a read-úthoz */
int  mnt_want_write_file(struct file *f) { (void)f; return 0; }
void mnt_drop_write_file(struct file *f) { (void)f; }
bool vfsgid_in_group_p(vfsgid_t g) { (void)g; return true; }
int  fs_param_is_bool(struct fs_context *fc, struct fs_parameter *p, struct fs_parse_result *r) { (void)fc;(void)p; r->boolean = true; return 0; }
int  fs_param_is_u32(struct fs_context *fc, struct fs_parameter *p, struct fs_parse_result *r) { (void)fc; r->uint_32 = p->string ? (unsigned)strtoul(p->string,0,0) : 0; return 0; }
int  fs_param_is_s32(struct fs_context *fc, struct fs_parameter *p, struct fs_parse_result *r) { (void)fc; r->int_32 = p->string ? (int)strtol(p->string,0,0) : 0; return 0; }
int  fs_param_is_string(struct fs_context *fc, struct fs_parameter *p, struct fs_parse_result *r) { (void)fc;(void)p;(void)r; return 0; }
int  fs_param_is_enum(struct fs_context *fc, struct fs_parameter *p, struct fs_parse_result *r) { (void)fc;(void)p; r->uint_32 = 0; return 0; }
int  fs_param_is_uid(struct fs_context *fc, struct fs_parameter *p, struct fs_parse_result *r) { (void)fc;(void)p; r->uid = GLOBAL_ROOT_UID; return 0; }
int  fs_param_is_gid(struct fs_context *fc, struct fs_parameter *p, struct fs_parse_result *r) { (void)fc;(void)p; r->gid = GLOBAL_ROOT_GID; return 0; }
/* fs_parse: a param->key-t megkeresi a spec-tömbben, és visszaadja a .opt-ot (a típusos
 * értékeket a result-ba — minimál). A flag-ek (type==NULL) érték nélkül. */
int  fs_parse(struct fs_context *fc, const struct fs_parameter_spec *d, struct fs_parameter *p, struct fs_parse_result *r)
{
	(void)fc;
	if (r) memset(r, 0, sizeof(*r));
	if (!d || !p || !p->key) return -2;
	for (const struct fs_parameter_spec *s = d; s->name; s++) {
		if (strcmp(s->name, p->key)) continue;
		if (r && p->string) { r->uint_64 = (u64)strtoull(p->string, 0, 0); r->uint_32 = (unsigned)r->uint_64; r->int_32 = (int)r->uint_64; r->boolean = r->uint_32 != 0; }
		return s->opt;
	}
	return -2;
}

/* __getname/__putname — útnév-puffer (PATH_MAX) */
char *__getname(void) { return malloc(4096); }
void  __putname(const char *name) { free((void *)name); }

/* blkdev-kérdezők (discard/méret) — stub */
sector_t bdev_nr_sectors(struct block_device *b) { (void)b; return 0; }
unsigned int bdev_max_discard_sectors(struct block_device *b) { (void)b; return 0; }
unsigned int bdev_discard_granularity(struct block_device *b) { (void)b; return 0; }
int sb_issue_discard(struct super_block *sb, sector_t a, sector_t n, gfp_t g, unsigned f) { (void)sb;(void)a;(void)n;(void)g;(void)f; return 0; }
int blockdev_direct_IO_simple(void) { return 0; }

/* ===== generic_* fájl/dir-műveletek ===== */
loff_t generic_file_llseek(struct file *file, loff_t offset, int whence)
{
	loff_t sz = file->f_inode ? file->f_inode->i_size : 0;
	if (whence == SEEK_END) offset += sz;
	else if (whence == SEEK_CUR) offset += file->f_pos;
	if (offset < 0) return -22;
	file->f_pos = offset; return offset;
}
ssize_t generic_read_dir(struct file *f, char __user *b, size_t s, loff_t *p) { (void)f;(void)b;(void)s;(void)p; return -21; }
int generic_setlease(struct file *f, int a, void **b, void **c) { (void)f;(void)a;(void)b;(void)c; return -22; }
int generic_file_mmap_prepare(struct vm_area_desc *desc) { (void)desc; return 0; }
int filemap_fault(struct vm_fault *vmf) { (void)vmf; return 0; }
void filemap_map_pages(struct vm_fault *vmf, unsigned long s, unsigned long e) { (void)vmf;(void)s;(void)e; }
int generic_file_fsync(struct file *f, loff_t s, loff_t e, int d) { (void)f;(void)s;(void)e;(void)d; return 0; }
long compat_ptr_ioctl(struct file *f, unsigned int c, unsigned long a)
{ return f->f_op && f->f_op->unlocked_ioctl ? f->f_op->unlocked_ioctl(f, c, a) : -25; }
ssize_t iter_file_splice_write(struct pipe_inode_info *p, struct file *f, loff_t *o, size_t l, unsigned int fl) { (void)p;(void)f;(void)o;(void)l;(void)fl; return 0; }
ssize_t filemap_splice_read(struct file *in, loff_t *ppos, struct pipe_inode_info *pipe, size_t len, unsigned int flags) { (void)in;(void)ppos;(void)pipe;(void)len;(void)flags; return 0; }

/* NFS-export segédek — stub */
int generic_encode_ino32_fh(struct inode *i, __u32 *fh, int *ml, struct inode *p) { (void)i;(void)fh;(void)ml;(void)p; return 1; }
struct dentry *generic_fh_to_dentry(struct super_block *sb, struct fid *fid, int fl, int ft, struct inode *(*gi)(struct super_block *, u64, u32)) { (void)sb;(void)fid;(void)fl;(void)ft;(void)gi; return NULL; }
struct dentry *generic_fh_to_parent(struct super_block *sb, struct fid *fid, int fl, int ft, struct inode *(*gi)(struct super_block *, u64, u32)) { (void)sb;(void)fid;(void)fl;(void)ft;(void)gi; return NULL; }

/* sched/signal segédek, ha a shim nem adja */
int fatal_signal_pending(void *t) { (void)t; return 0; }
int need_resched(void) { return 0; }
long io_schedule_timeout(long t) { return t; }
ssize_t generic_file_read_iter(struct kiocb *iocb, struct iov_iter *iter) { (void)iocb;(void)iter; return 0; }
ssize_t generic_file_write_iter(struct kiocb *iocb, struct iov_iter *iter) { (void)iocb;(void)iter; return 0; }

/* ratelimit + folio/page-cache stubok (a 2.+ FS-ekhez, főleg link-kielégítés) */
int __ratelimit(struct ratelimit_state *rs) { (void)rs; return 1; }

/* kmem_cache_create diszpécser-wrapperek: a header makróvá tette, de a kernel-shim
 * a valódi kmem_cache_create függvényt exportálja — #undef-fel elérjük. */
#undef kmem_cache_create
extern struct kmem_cache *kmem_cache_create(const char *name, size_t size, size_t align, unsigned long flags, void (*ctor)(void *));
struct kmem_cache *__kmem_cache_create_old(const char *name, size_t size, size_t align, unsigned long flags, void (*ctor)(void *))
{ return kmem_cache_create(name, size, align, flags, ctor); }
struct kmem_cache *__kmem_cache_create_args(const char *name, unsigned int size, struct kmem_cache_args *args, unsigned int flags)
{ return kmem_cache_create(name, size, args ? args->align : 0, flags, args ? args->ctor : 0); }

/* ===== exfat/ntfs3 futásidő-stubok (mount+ls+cat: read-út valódi, write-út stub) ===== */
#include <linux/uio.h>
void balance_dirty_pages_ratelimited(struct address_space *m) { (void)m; }
int  bdev_freeze(struct block_device *b) { (void)b; return 0; }
int  bdev_thaw(struct block_device *b) { (void)b; return 0; }
unsigned int bdev_logical_block_size(struct block_device *b) { (void)b; return 512; }
int  bh_read(struct buffer_head *bh, blk_opf_t op_flags) { (void)op_flags; return bh && (bh->b_state & (1UL<<BH_Uptodate)) ? 1 : 0; }
/* ===== VALÓDI írási út: block_write_begin + generic_write_end a blokk-backenden =====
 * A driver a_ops->write_begin-je hív minket get_block-kal (cluster-allokáció), mi
 * lefoglaljuk a fizikai blokkokat, beolvassuk a meglévő tartalmat (részleges írás),
 * majd generic_write_end visszaírja az image-re és frissíti az inode-méretet. */
#define UK_WBLK 16
struct uk_wfolio {
	struct folio fo;                  /* ELSŐ tag — a driver ezt kapja folio*-ként */
	char data[8192];                  /* a lap adatpuffere (folio_address ezt adja) */
	struct buffer_head bh[UK_WBLK];   /* blokkonként egy bh, fizikai blokkszámmal */
	int nbh; unsigned bs; struct inode *inode; pgoff_t index;
};
int block_write_begin(struct address_space *m, loff_t pos, unsigned len, struct folio **f, get_block_t *gb)
{
	struct inode *inode = m->host;
	unsigned bs = inode->i_sb->s_blocksize ? inode->i_sb->s_blocksize : g_blocksize;
	struct uk_wfolio *w = calloc(1, sizeof(*w));
	if (!w) return -12;
	w->bs = bs; w->inode = inode;
	w->index = pos >> 12;                     /* PAGE_SHIFT=12 */
	w->fo._priv = w->data;                    /* folio_address(folio) -> w->data */
	w->fo.mapping = m; w->fo.index = w->index;
	unsigned per = 4096 / bs; if (per > UK_WBLK) per = UK_WBLK; if (!per) per = 1;
	sector_t base_iblock = (sector_t)w->index * per;
	/* CSAK a ténylegesen írt [from,to) tartomány blokkjait foglaljuk (mint a kernel
	 * __block_write_begin-je) — különben az egész 4096B-os lapra get_block(create=1)
	 * fölös fürtöket allokálna (pl. egy 4 bájtos fájlhoz 2 fürtöt → fsck "cluster chain
	 * length > N"). A nem érintett bh-k b_blocknr=0 maradnak → a write_end kihagyja őket. */
	unsigned from = (unsigned)(pos & 4095), to = from + len; if (to > 4096) to = 4096;
	unsigned bfrom = from / bs, bto = (to + bs - 1) / bs;
	for (unsigned b = 0; b < per; b++) {
		struct buffer_head *bh = &w->bh[b];
		bh->b_size = bs; bh->b_data = w->data + b * bs; bh->b_bdev_sb = inode->i_sb;
		if (b < bfrom || b >= bto) continue;          /* ezt a blokkot nem írjuk → nem foglalunk */
		if (gb) gb(inode, base_iblock + b, bh, 1);   /* cluster-allokáció (create=1) */
		if (bh->b_blocknr && (bh->b_state & (1UL << BH_Mapped)))
			bdev_pread(bh->b_data, bs, (off_t)bh->b_blocknr * bs);  /* meglévő tartalom */
	}
	w->nbh = per;
	*f = &w->fo;
	return 0;
}
void folio_zero_new_buffers(struct folio *folio, size_t from, size_t to) { (void)folio;(void)from;(void)to; }

/* ===== ext4 buffered írás (write_begin_get_folio + create_empty_buffers + block_write_end) ===== */
/* write_begin_get_folio: az ext4_write_begin ezen kéri a folio-t. Adatpuffer + index. */
struct folio *write_begin_get_folio(const struct kiocb *iocb, struct address_space *mapping, pgoff_t index, size_t len)
{
	(void)iocb; (void)len;
	struct folio *fo = calloc(1, sizeof(*fo));
	if (!fo) return (struct folio *)ERR_PTR(-12);
	fo->_priv = calloc(1, 4096);
	if (!fo->_priv) { free(fo); return (struct folio *)ERR_PTR(-12); }
	fo->mapping = mapping; fo->index = index;
	fo->page._priv = fo->_priv; fo->page.mapping = mapping; fo->page.index = index;
	fo->flags |= 1UL;            /* UK_PG_LOCKED — az ext4_block_write_begin BUG_ON-ozik rá */
	return fo;
}
/* create_empty_buffers: buffer_head-láncot fűz a folio-ra (blokkonként egy bh, a b_data a
 * folio adatpufferébe mutat — közös memória). A láncot a folio->uk_bh-ba tárolja. */
struct buffer_head *create_empty_buffers(struct folio *folio, unsigned long blocksize, unsigned long b_state)
{
	struct inode *inode = folio->mapping ? folio->mapping->host : 0;
	unsigned bs = blocksize ? blocksize : g_blocksize;
	unsigned per = 4096 / bs; if (!per) per = 1;
	char *data = folio_address(folio);
	struct buffer_head *head = 0, *prev = 0;
	for (unsigned b = 0; b < per; b++) {
		struct buffer_head *bh = calloc(1, sizeof(*bh));
		if (!bh) break;
		bh->b_data = data + b * bs; bh->b_size = bs; bh->b_count = 1;
		bh->b_state = b_state;   /* NEM Mapped — különben az ext4 nem allokál blokkot! */
		bh->b_bdev_sb = inode ? inode->i_sb : 0;
		bh->b_folio = folio;
		if (!head) head = bh; else prev->b_this_page = bh;
		prev = bh;
	}
	if (prev) prev->b_this_page = head;   /* körkörös */
	folio->uk_bh = head;
	return head;
}
void folio_create_empty_buffers(struct folio *folio, unsigned long blocksize, unsigned long b_state)
{ create_empty_buffers(folio, blocksize, b_state); }
/* block_write_end: a folio-adat a buffer-ekkel közös → a leképzett (b_blocknr) buffereket
 * kiírjuk a backendre, és frissítjük az i_size-t. copied bájtot ad vissza. */
int block_write_end(loff_t pos, unsigned len, unsigned copied, struct folio *folio)
{
	(void)len;
	struct inode *inode = folio->mapping ? folio->mapping->host : 0;
	struct buffer_head *head = folio->uk_bh, *bh = head;
	if (bh) do {
		if (bh->b_blocknr && (bh->b_state & (1UL << BH_Mapped)) && bdev_active())
			bdev_pwrite(bh->b_data, bh->b_size, (off_t)bh->b_blocknr * bh->b_size);
		bh = bh->b_this_page;
	} while (bh && bh != head);
	if (inode && pos + (loff_t)copied > inode->i_size) inode->i_size = pos + copied;
	return copied;
}
void d_drop(struct dentry *d) { (void)d; }
void d_rehash(struct dentry *d) { (void)d; }
int  d_unhashed(const struct dentry *d) { (void)d; return 0; }
void file_accessed(struct file *f) { (void)f; }
int  filemap_page_mkwrite(struct vm_fault *vmf) { (void)vmf; return 0; }
void *folio_address(const void *folio) { return folio ? ((struct folio *)folio)->_priv : 0; }
ssize_t __generic_file_write_iter(struct kiocb *iocb, struct iov_iter *from) { (void)iocb;(void)from; return -95; }
ssize_t generic_write_checks(struct kiocb *iocb, struct iov_iter *from) { (void)iocb; return from ? (ssize_t)from->count : 0; }
ssize_t generic_write_sync(struct kiocb *iocb, ssize_t count) { (void)iocb; return count; }
unsigned int hash_32(unsigned int val, unsigned int bits) { return (val*0x61C88647u) >> (32-bits); }
int  inode_newsize_ok(const struct inode *inode, loff_t offset) { (void)inode;(void)offset; return 0; }
int  inode_trylock(struct inode *inode) { (void)inode; return 1; }
unsigned long iov_iter_alignment(const struct iov_iter *i) { (void)i; return 0; }
void iov_iter_revert(struct iov_iter *i, size_t bytes) { (void)i;(void)bytes; }
size_t iov_iter_zero(size_t bytes, struct iov_iter *i) { (void)i; return bytes; }
void *memchr_inv(const void *start, int c, size_t bytes) { const unsigned char *p=start; for(size_t i=0;i<bytes;i++) if(p[i]!=(unsigned char)c) return (void*)(p+i); return 0; }
time64_t mktime64(const unsigned int y, const unsigned int mo, const unsigned int d, const unsigned int h, const unsigned int mi, const unsigned int s)
{ unsigned int mon=mo, year=y; if((int)(mon-=2)<=0){mon+=12;year-=1;} return ((((time64_t)(year/4-year/100+year/400+367*mon/12+d)+year*365-719499)*24+h)*60+mi)*60+s; }
int  simple_fsync_noflush(struct file *f, loff_t a, loff_t b, int d) { (void)f;(void)a;(void)b;(void)d; return 0; }
struct timespec64 simple_inode_init_ts(struct inode *inode) { struct timespec64 t={0,0}; if(inode){inode->i_atime=t;inode->i_mtime=t;inode->__i_ctime=t;} return t; }
void simple_rename_timestamp(struct inode *a, struct dentry *b, struct inode *c, struct dentry *d) { (void)a;(void)b;(void)c;(void)d; }
/* sync_blockdev: a registry MINDEN dirty bufferét kiírja (a held GD/SB-buffereket is). */
int  sync_blockdev(struct block_device *b) { (void)b; for (struct uk_bhnode *n = g_bhlist; n; n = n->next) bh_writeback(n->bh); return 0; }
int  __sync_dirty_buffer(struct buffer_head *bh, int op) { (void)op; return sync_dirty_buffer(bh); }
int  utf8_to_utf32(const char *s, int len, unsigned int *pu) { if(len<1) return -1; *pu=(unsigned char)*s; return 1; }
vm_fault_t vmf_fs_error(int err) { (void)err; return VM_FAULT_SIGBUS; }

/* uk_current_task + uk_init_fs MOSTANTÓL a core shimben (shim/core/sched.c) — hogy a wifi-stack is megkapja.
 * (A current->fs->umask-ot az exfat/ext4 init_fs_context olvassa; a definíció a libkernel_shim-ben van.) */

/* ===== C-API az LD_PRELOAD-bridge-hez (preload_fs.c) — lásd include/uk_fs_api.h ===== */
#include "uk_fs_api.h"
extern int ukernel_run_module_inits(void);
static int g_api_inited;

struct uk_fcache;
static struct uk_fcache *g_fcache;   /* fwd-decl: a ukfs_remount törli (a teljes def lentebb) */

int ukfs_mount(const char *fstype, const char *img)
{
	if (!g_api_inited) { ukernel_run_module_inits(); g_api_inited = 1; }
	/* Start from a CLEAN cache. ukfsd is one long-lived process; a guest
	 * umount→mount cycle (or remounting a different device) reuses these globals,
	 * so stale buffer/inode/page caches from a previous mount would leak in and
	 * make lookups read pre-fsck / pre-write block contents — flaky ENOENT/EIO.
	 * Drop them (don't free: leaked driver refs must not become use-after-free),
	 * exactly like ukfs_remount. The i_sb guard already separates inodes by sb,
	 * but the buffer cache is keyed by block number and MUST be dropped. */
	g_bhlist = NULL;
	memset(g_ihash, 0, sizeof(g_ihash));
	g_fcache = NULL;
	g_blocksize = 512;
	if (g_bdev_fd >= 0) { close(g_bdev_fd); g_bdev_fd = -1; }
	if (g_bdev_sock >= 0) { bsock_close(); g_bdev_sock = -1; }
	g_api_root = uk_mount(fstype, img);
	return (g_api_root && g_api_root->d_inode) ? 0 : -1;
}

/* ===== ukfs_remount: a lemez-állapot FRISS újraolvasása (a párhuzamos íráshoz) =====
 * A párhuzamos írók külön driver-példánnyal, STALE cache-sel ütköző blokkokat allokálnak →
 * korrupció. A bridge minden write-session ELEJÉN (a kizárólagos zár alatt) ezt hívja, hogy a
 * legfrissebb lemez-állapotot lássa. A régi cache-eket ELDOBJUK (NEM free — a leakelt driver-
 * referenciák ne legyenek use-after-free); memóriát szivárogtat, de a párhuzamos írás ritka és a
 * processzek rövid életűek. (ext4/jbd2-nél a journal-szál is leak → ott a bridge óvatosabban hív.) */
int ukfs_remount(const char *fstype, const char *img)
{
	g_bhlist = NULL;                       /* stale buffer-cache eldobása */
	memset(g_ihash, 0, sizeof(g_ihash));   /* stale inode-hash eldobása */
	g_fcache = NULL;                       /* stale page-cache eldobása */
	g_blocksize = 512;
	if (g_bdev_fd >= 0) { close(g_bdev_fd); g_bdev_fd = -1; }
	if (g_bdev_sock >= 0) { bsock_close(); g_bdev_sock = -1; }
	g_api_root = uk_mount(fstype, img);    /* friss fill_super → friss sb/sbi/root */
	return (g_api_root && g_api_root->d_inode) ? 0 : -1;
}

/* ===== ukfs_statfs: a fájlrendszer-statisztika (df / statvfs) a driver s_op->statfs-én át ===== */
int ukfs_statfs(unsigned long *bsize, unsigned long long *blocks, unsigned long long *bfree,
                unsigned long long *bavail, unsigned long long *files, unsigned long long *ffree,
                long *namelen, long *frsize, long *ftype)
{
	if (!g_mount_sb || !g_mount_sb->s_op || !g_mount_sb->s_op->statfs || !g_api_root) return -1;
	struct kstatfs ks; memset(&ks, 0, sizeof(ks));
	if (g_mount_sb->s_op->statfs(g_api_root, &ks)) return -1;
	if (bsize)   *bsize   = ks.f_bsize;
	if (blocks)  *blocks  = ks.f_blocks;
	if (bfree)   *bfree   = ks.f_bfree;
	if (bavail)  *bavail  = ks.f_bavail;
	if (files)   *files   = ks.f_files;
	if (ffree)   *ffree   = ks.f_ffree;
	if (namelen) *namelen = ks.f_namelen ? ks.f_namelen : 255;
	if (frsize)  *frsize  = ks.f_frsize ? ks.f_frsize : ks.f_bsize;
	if (ftype)   *ftype   = ks.f_type;
	return 0;
}

/* dinamikusan növő lista — a htree (dir_index) könyvtárak több ezer bejegyzést tartalmaznak,
 * a régi fix 256-os tömb csonkolta a nagy könyvtárak listázását. */
static struct ukfs_dirent *g_api_dir;
static int g_api_dirn, g_api_cap;
static int api_collect(struct dir_context *ctx, const char *name, int namelen, loff_t off, u64 ino, unsigned type)
{
	(void)ctx; (void)off;
	if (namelen == 1 && name[0] == '.') return 0;
	if (namelen == 2 && name[0] == '.' && name[1] == '.') return 0;
	if (namelen >= 256) return 0;
	if (g_api_dirn >= g_api_cap) {
		int nc = g_api_cap ? g_api_cap * 2 : 256;
		struct ukfs_dirent *nn = realloc(g_api_dir, (size_t)nc * sizeof(*nn));
		if (!nn) return 0;
		g_api_dir = nn; g_api_cap = nc;
	}
	memcpy(g_api_dir[g_api_dirn].name, name, namelen);
	g_api_dir[g_api_dirn].name[namelen] = 0;
	g_api_dir[g_api_dirn].ino = ino;
	g_api_dir[g_api_dirn].type = (type == DT_DIR) ? 1 : 2;
	g_api_dir[g_api_dirn].size = 0;
	g_api_dirn++;
	return 0;
}

static struct dentry *api_lookup(const char *name);   /* fwd: az api_walk később van definiálva */

/* ukfs_list_dir: egy könyvtár (path; üres="" = gyökér) tartalmának listázása a VALÓDI
 * driver iterate_shared-jával — így az alkönyvtárak is bejárhatók. */
int ukfs_list_dir(const char *path, struct ukfs_dirent **out)
{
	if (!g_api_root) { *out = g_api_dir; return 0; }
	g_api_dirn = 0;
	struct dentry *dd = (!path || !*path) ? g_api_root : api_lookup(path);
	if (!dd || !dd->d_inode) { *out = g_api_dir; return 0; }
	struct inode *dir = dd->d_inode;
	if (!dir->i_fop || !dir->i_fop->iterate_shared) { *out = g_api_dir; return 0; }
	struct file f; memset(&f, 0, sizeof(f));
	f.f_inode = dir; f.f_path.dentry = dd; f.f_op = dir->i_fop; f.f_mapping = dir->i_mapping;
	/* ext4: a dir_private_info-t az open allokálja (file->private_data) — különben crash */
	if (dir->i_fop->open) dir->i_fop->open(dir, &f);
	struct dir_context ctx = { .actor = api_collect, .pos = 0 };
	dir->i_fop->iterate_shared(&f, &ctx);
	if (dir->i_fop->release) dir->i_fop->release(dir, &f);
	/* méretek feltöltése a TELJES úttal (a levél-név a listázott könyvtárhoz relatív) */
	for (int i = 0; i < g_api_dirn; i++) {
		if (g_api_dir[i].type != 2) continue;
		char full[1024];
		if (path && *path) snprintf(full, sizeof(full), "%s/%s", path, g_api_dir[i].name);
		else { strncpy(full, g_api_dir[i].name, sizeof(full)-1); full[sizeof(full)-1]=0; }
		g_api_dir[i].size = ukfs_file_size(full);
	}
	*out = g_api_dir;
	return g_api_dirn;
}
int ukfs_list(struct ukfs_dirent **out) { return ukfs_list_dir("", out); }

/* egyetlen név lookupja egy adott könyvtár-inode-ban */
static struct dentry *lookup_one(struct inode *dir, struct dentry *parent, const char *name)
{
	if (!dir || !dir->i_op || !dir->i_op->lookup) return 0;
	struct dentry *de = calloc(1, sizeof(*de));
	if (!de) return 0;
	de->d_sb = dir->i_sb; de->d_parent = parent;
	de->d_name.name = (const unsigned char *)strdup(name);
	de->d_name.len = strlen(name);
	struct dentry *res = dir->i_op->lookup(dir, de, 0);
	if (IS_ERR(res)) return de;   /* lookup-hiba (pl. -ENOENT): de->d_inode marad NULL */
	return res ? res : de;
}

/* path-bejárás a gyökértől: a köztes komponenseket lookupolja (mind könyvtár kell legyen),
 * a *parent-be a LEVÉL szülő-könyvtárának inode-ját + dentryjét teszi, a *leafname-be a levél
 * nevét. Visszaadja a levél dentryjét (d_inode NULL, ha még nem létezik), vagy NULL-t (rossz út). */
static char g_leafbuf[256];
static struct dentry *api_walk(const char *path, struct inode **parent, struct dentry **pdentry, const char **leafname)
{
	if (!g_api_root || !path) return 0;
	struct dentry *cur = g_api_root;
	char buf[1024]; strncpy(buf, path, sizeof(buf)-1); buf[sizeof(buf)-1] = 0;
	char *save = 0, *comp = strtok_r(buf, "/", &save);
	if (!comp) return 0;
	while (comp) {
		char *next = strtok_r(0, "/", &save);
		/* "." -> current dir; ".." -> parent (clamped at the mount root). Resolve
		 * these logically rather than via a driver lookup: a FAT *root* directory
		 * has no on-disk "."/".." entries, so STAT/open of "/." or "/.." would
		 * spuriously fail (and ls would then show the mount root with host attrs). */
		if (strcmp(comp, ".") == 0 || strcmp(comp, "..") == 0) {
			struct dentry *tgt = (comp[1] == '.')
				? (cur->d_parent ? cur->d_parent : cur)   /* ".." */
				: cur;                                     /* "."  */
			if (!next) {                       /* ez a LEVÉL */
				struct dentry *pd = tgt->d_parent ? tgt->d_parent : tgt;
				strncpy(g_leafbuf, comp, sizeof(g_leafbuf)-1); g_leafbuf[sizeof(g_leafbuf)-1]=0;
				if (parent) *parent = pd->d_inode;
				if (pdentry) *pdentry = pd;
				if (leafname) *leafname = g_leafbuf;
				return tgt;
			}
			cur = tgt; comp = next; continue;
		}
		if (!next) {                       /* ez a LEVÉL */
			strncpy(g_leafbuf, comp, sizeof(g_leafbuf)-1); g_leafbuf[sizeof(g_leafbuf)-1]=0;
			if (parent) *parent = cur->d_inode;
			if (pdentry) *pdentry = cur;
			if (leafname) *leafname = g_leafbuf;
			return lookup_one(cur->d_inode, cur, comp);
		}
		/* köztes komponens: le kell tudni lépni (könyvtár) */
		struct dentry *d = lookup_one(cur->d_inode, cur, comp);
		if (!d || !d->d_inode || (d->d_inode->i_mode & 0170000) != 0040000) {
			fprintf(stderr, "ukfsd: api_walk FAIL comp='%s' path='%s' d=%p inode=%p mode=%o\n",
			        comp, path, (void*)d, d ? (void*)d->d_inode : 0,
			        (d && d->d_inode) ? (unsigned)d->d_inode->i_mode : 0u);
			fflush(stderr);
			return 0;
		}
		cur = d;
		comp = next;
	}
	return 0;
}

static struct dentry *api_lookup(const char *name)
{
	return api_walk(name, 0, 0, 0);
}

long ukfs_file_size(const char *name)
{
	struct dentry *d = api_lookup(name);
	return (d && d->d_inode) ? (long)d->d_inode->i_size : -1;
}

/* ===== ukfs_seek_data_hole: a következő ADAT (is_data=1, SEEK_DATA) ill. LYUK (is_data=0,
 * SEEK_HOLE) pozíciója `offset`-től, a VALÓDI blokk-allokáció (a_ops->bmap) alapján — így a
 * sparse-tudatos eszközök (cp --sparse, tar -S) látják a uKernel-fájl lyuk-térképét.
 * Visszaad: a pozíció (>=0), vagy -6 (-ENXIO: offset >= méret, ill. SEEK_DATA után nincs adat),
 * -2 (-ENOENT). A bridge a temp-fd pozícióját is erre szinkronizálja. */
long long ukfs_seek_data_hole(const char *name, long long offset, int is_data)
{
	struct dentry *d = api_lookup(name);
	if (!d || !d->d_inode) return -2;
	struct inode *in = d->d_inode;
	loff_t size = in->i_size;
	if (offset < 0) offset = 0;
	if (offset >= size) return -6;                  /* -ENXIO: a méreten túl */
	struct address_space *m = in->i_mapping;
	/* nincs bmap VAGY rezidens fájl (i_blocks==0, pl. kis ntfs3): az EGÉSZ fájl ADAT (nincs lyuk) */
	if (!m || !m->a_ops || !m->a_ops->bmap || in->i_blocks == 0)
		return is_data ? offset : (long long)size;  /* SEEK_DATA: itt adat; SEEK_HOLE: csak a végén */
	unsigned bs = in->i_sb->s_blocksize ? in->i_sb->s_blocksize : g_blocksize;
	loff_t pos = offset;
	while (pos < size) {
		sector_t fblock = pos / bs;
		sector_t phys = m->a_ops->bmap(m, fblock);
		int data_here = (phys != 0);
		if (is_data && data_here) return pos;        /* megtaláltuk a következő adatot */
		if (!is_data && !data_here) return pos;       /* megtaláltuk a következő lyukat */
		pos = (loff_t)(fblock + 1) * bs;             /* a következő blokk eleje */
	}
	/* a fájl végéig értünk */
	return is_data ? -6 : (long long)size;          /* SEEK_DATA: nincs több adat; SEEK_HOLE: implicit lyuk a végén */
}

long ukfs_read_file(const char *name, char *buf, size_t maxlen, long pos)
{
	struct dentry *d = api_lookup(name);
	if (!d || !d->d_inode) return -1;
	return uk_read(d, buf, maxlen, pos);
}

int ukfs_path_kind(const char *name)
{
	if (!name || !*name) return 0;            /* a mountpont gyökere = könyvtár */
	struct dentry *d = api_lookup(name);
	if (!d || !d->d_inode) return -1;
	unsigned m = d->d_inode->i_mode & 0170000;
	if (m == 0040000) return 2;               /* alkönyvtár */
	if (m == 0120000) return 3;               /* symlink (a bridge csak ekkor old fel — ntfs3-stabilitás) */
	return 1;                                 /* fájl */
}

/* uk_flush_meta: a módosított inode ÉS a szülő-könyvtár metaadatának perzisztálása
 * (write_inode + sync_fs + freeze/checkpoint + sync_blockdev) — közös az írás és a mkdir között. */
static void uk_flush_meta(struct inode *inode, struct inode *dir)
{
	struct super_block *wsb = inode->i_sb;
	const char *fl = getenv("UK_WRFLUSH");
	int do_wi = !fl || strchr(fl, 'i'), do_wd = !fl || strchr(fl, 'd'), do_sy = !fl || strchr(fl, 's');
	int do_fz = !fl || strchr(fl, 'f');
	if (wsb->s_op && wsb->s_op->write_inode) {
		struct writeback_control wbc; memset(&wbc, 0, sizeof(wbc)); wbc.sync_mode = WB_SYNC_ALL;
		if (do_wi) wsb->s_op->write_inode(inode, &wbc);
		if (do_wd && dir) wsb->s_op->write_inode(dir, &wbc);
	}
	if (do_sy && wsb->s_op && wsb->s_op->sync_fs) wsb->s_op->sync_fs(wsb, 1);
	if (do_fz && wsb->s_op && wsb->s_op->freeze_fs) {
		wsb->s_op->freeze_fs(wsb);
		if (wsb->s_op->unfreeze_fs) wsb->s_op->unfreeze_fs(wsb);
	}
	if (do_sy) sync_blockdev(wsb->s_bdev);
}

/* Refresh the inode (+ optional dir) dir-entry in the buffer cache WITHOUT a
 * block-device sync — the cheap per-write() path; ukfs_sync_path() flushes later. */
static void uk_meta_inode_only(struct inode *inode, struct inode *dir)
{
	struct super_block *wsb = inode->i_sb;
	if (wsb->s_op && wsb->s_op->write_inode) {
		struct writeback_control wbc; memset(&wbc, 0, sizeof(wbc)); wbc.sync_mode = WB_SYNC_NONE;
		wsb->s_op->write_inode(inode, &wbc);
		if (dir) wsb->s_op->write_inode(dir, &wbc);
	}
}

/* ===== ukfs_mkdir: alkönyvtár létrehozása a VALÓDI driver i_op->mkdir-jával ===== */
long ukfs_mkdir(const char *name)
{
	if (!g_api_root) return -1;
	struct inode *dir = 0; struct dentry *pdent = 0; const char *leaf = 0;
	struct dentry *ex = api_walk(name, &dir, &pdent, &leaf);
	if (!dir || !leaf) return -2;
	if (ex && ex->d_inode) return -17;        /* már létezik (-EEXIST) */
	if (!dir->i_op || !dir->i_op->mkdir) return -2;
	struct dentry *nde = calloc(1, sizeof(*nde));
	if (!nde) return -3;
	nde->d_sb = dir->i_sb; nde->d_parent = pdent;
	nde->d_name.name = (const unsigned char *)strdup(leaf);
	nde->d_name.len = strlen(leaf);
	struct dentry *r = dir->i_op->mkdir(&nop_mnt_idmap, dir, nde, 0040755);
	if (IS_ERR(r)) return PTR_ERR(r);
	struct inode *inode = nde->d_inode ? nde->d_inode : (r ? r->d_inode : 0);
	if (!inode) {                              /* visszakeressük, ha a mkdir nem instanciált */
		struct dentry *lr = dir->i_op->lookup(dir, nde, 0);
		inode = lr ? lr->d_inode : 0;
	}
	if (!inode) return -4;
	uk_flush_meta(inode, dir);                 /* az új könyvtár + a szülő perzisztálása */
	return 0;
}

/* ===== special fájlok: dev-kódolás + init_special_inode (eddig no-op stubok voltak) ===== */
u32 new_encode_dev(dev_t dev)
{ unsigned major = dev >> 20, minor = dev & 0xfffff; return (minor & 0xff) | (major << 8) | ((minor & ~0xffU) << 12); }
dev_t new_decode_dev(u32 dev)
{ unsigned major = (dev & 0xfff00) >> 8, minor = (dev & 0xff) | ((dev >> 12) & 0xfff00); return ((dev_t)major << 20) | minor; }
/* init_special_inode: a mode-ot ÉS az rdev-et az inode-ba (a no-op stub miatt az rdev elveszett —
 * a FIFO/socket-nél mindegy, de a device-node-nál a major:minor onnan jön). A driver write_inode-ja
 * a mode-ot (típus-bitekkel) + rdev-et a lemezre írja → a special fájl perzisztál. */
void init_special_inode(struct inode *inode, umode_t mode, dev_t rdev)
{ if (!inode) return; inode->i_mode = mode; inode->i_rdev = rdev; }

/* ===== ukfs_mknod: special fájl (FIFO/device-node/socket) a VALÓDI driver i_op->mknod-jával ===== */
long ukfs_mknod(const char *name, unsigned int mode, unsigned long dev)
{
	if (!g_api_root) return -1;
	struct inode *dir = 0; struct dentry *pdent = 0; const char *leaf = 0;
	struct dentry *ex = api_walk(name, &dir, &pdent, &leaf);
	if (!dir || !leaf) return -2;
	if (ex && ex->d_inode) return -17;                 /* már létezik (-EEXIST) */
	if (!dir->i_op || !dir->i_op->mknod) return -38;   /* a FS nem támogat special fájlt (-ENOSYS) */
	struct dentry *nde = calloc(1, sizeof(*nde));
	if (!nde) return -12;
	nde->d_sb = dir->i_sb; nde->d_parent = pdent;
	nde->d_name.name = (const unsigned char *)strdup(leaf);
	nde->d_name.len = strlen(leaf);
	/* a userspace dev-et a kernel dev_t-re fordítjuk (FIFO/socket: dev=0 → mindegy) */
	int e = dir->i_op->mknod(&nop_mnt_idmap, dir, nde, mode, new_decode_dev((u32)dev));
	if (e) return e;
	struct inode *inode = nde->d_inode ? nde->d_inode : 0;
	if (!inode) { struct dentry *lr = dir->i_op->lookup(dir, nde, 0); inode = lr ? lr->d_inode : 0; }
	if (inode) uk_flush_meta(inode, dir); else uk_flush_meta(dir, 0);
	return 0;
}

/* ===== ukfs_unlink: fájl törlése a VALÓDI driver i_op->unlink-jával ===== */
long ukfs_unlink(const char *name)
{
	if (!g_api_root) return -1;
	struct inode *dir = 0; struct dentry *pdent = 0; const char *leaf = 0;
	struct dentry *de = api_walk(name, &dir, &pdent, &leaf);
	if (!dir || !de || !de->d_inode) return -2;        /* nincs ilyen (-ENOENT) */
	if ((de->d_inode->i_mode & 0170000) == 0040000) return -21;  /* könyvtár -> rmdir (-EISDIR) */
	if (!dir->i_op || !dir->i_op->unlink) return -1;
	de->d_parent = pdent;
	struct inode *target = de->d_inode;
	int e = dir->i_op->unlink(dir, de);
	if (e) return e;
	/* eviction: a kernelben iput-kor fut; a no-op iput miatt itt KÉZZEL hívjuk — ez állítja az
	 * i_dtime-ot és szabadítja az inode-ot a bitmapben + a group-desc-számlálót (ext4_evict_inode
	 * a nlink==0-t maga ellenőrzi). Enélkül: fsck "Deleted inode has zero dtime" + count wrong. */
	if (target && dir->i_sb->s_op && dir->i_sb->s_op->evict_inode)
		dir->i_sb->s_op->evict_inode(target);
	remove_inode_hash(target);                         /* drop from icache: its slot may be reused */
	uk_flush_meta(dir, 0);                              /* a szülő-könyvtár (bejegyzés törölve) + bitmapek */
	return 0;
}

/* ===== ukfs_rmdir: ÜRES alkönyvtár törlése a VALÓDI driver i_op->rmdir-jával ===== */
long ukfs_rmdir(const char *name)
{
	if (!g_api_root) return -1;
	struct inode *dir = 0; struct dentry *pdent = 0; const char *leaf = 0;
	struct dentry *de = api_walk(name, &dir, &pdent, &leaf);
	if (!dir || !de || !de->d_inode) return -2;        /* nincs ilyen (-ENOENT) */
	if ((de->d_inode->i_mode & 0170000) != 0040000) return -20;  /* nem könyvtár (-ENOTDIR) */
	if (!dir->i_op || !dir->i_op->rmdir) return -1;
	de->d_parent = pdent;
	struct inode *target = de->d_inode;
	int e = dir->i_op->rmdir(dir, de);                 /* a driver ellenőrzi az ürességet (-ENOTEMPTY) */
	if (e) return e;
	if (target && dir->i_sb->s_op && dir->i_sb->s_op->evict_inode)
		dir->i_sb->s_op->evict_inode(target);          /* dtime + bitmap + dir-count (lásd unlink) */
	remove_inode_hash(target);                         /* drop from icache: its slot may be reused */
	uk_flush_meta(dir, 0);
	return 0;
}

/* ===== ukfs_rename: átnevezés/áthelyezés a VALÓDI driver i_op->rename-jával ===== */
long ukfs_rename(const char *oldpath, const char *newpath)
{
	if (!g_api_root) return -1;
	struct inode *odir = 0, *ndir = 0; struct dentry *opd = 0, *npd = 0;
	const char *oleaf = 0, *nleaf = 0;
	struct dentry *ode = api_walk(oldpath, &odir, &opd, &oleaf);
	if (!odir || !ode || !ode->d_inode) return -2;     /* a forrás nincs (-ENOENT) */
	char osave[256]; strncpy(osave, oleaf, sizeof(osave)-1); osave[sizeof(osave)-1]=0;  /* g_leafbuf-ot a 2. walk felülírja */
	struct dentry *nde = api_walk(newpath, &ndir, &npd, &nleaf);
	if (!ndir || !nleaf) return -1;                    /* rossz cél-út */
	if (!odir->i_op || !odir->i_op->rename) return -1;
	ode->d_parent = opd;
	ode->d_name.name = (const unsigned char *)strdup(osave);
	ode->d_name.len = strlen(osave);
	struct inode *replaced = nde ? nde->d_inode : 0;   /* FELÜLÍRÁS: a cél már létezett */
	if (!nde) { nde = calloc(1, sizeof(*nde)); if (!nde) return -3; }
	nde->d_sb = ndir->i_sb; nde->d_parent = npd;
	nde->d_name.name = (const unsigned char *)strdup(nleaf);
	nde->d_name.len = strlen(nleaf);
	struct inode *moved = ode->d_inode;
	int e = odir->i_op->rename(&nop_mnt_idmap, odir, ode, ndir, nde, 0);
	if (e) return e;
	/* ha a rename felülírt egy meglévő célt, a régi cél-inode-ot evictálni kell (dtime + bitmap),
	 * mint az unlinknál — különben fsck "Deleted inode has zero dtime" / count wrong. */
	if (replaced && replaced != moved && ndir->i_sb->s_op && ndir->i_sb->s_op->evict_inode) {
		ndir->i_sb->s_op->evict_inode(replaced);
		remove_inode_hash(replaced);                   /* overwritten target: its slot may be reused */
	}
	/* a MOZGATOTT fájl inode-ját is ki kell írni: vfat/ntfs3-nál a méret/cluster a DIR-ENTRYBEN
	 * van (az új i_pos-on), amit a write_inode (fat_write_inode) ír — különben a cél 0 méretű lesz. */
	if (moved && ndir->i_sb->s_op && ndir->i_sb->s_op->write_inode) {
		struct writeback_control wbc; memset(&wbc, 0, sizeof(wbc)); wbc.sync_mode = WB_SYNC_ALL;
		ndir->i_sb->s_op->write_inode(moved, &wbc);
	}
	uk_flush_meta(odir, ndir != odir ? ndir : 0);      /* a forrás- ÉS cél-könyvtár perzisztálása */
	return 0;
}

/* uk_setattr: közös setattr-hívás (chmod/chown) — iattr a megadott mezőkkel + perzisztálás */
static long uk_setattr(const char *name, struct iattr *ia)
{
	if (!g_api_root) return -1;
	struct inode *dir = 0; struct dentry *pdent = 0; const char *leaf = 0;
	struct dentry *de = api_walk(name, &dir, &pdent, &leaf);
	if (!de || !de->d_inode) return -2;                /* nincs ilyen (-ENOENT) */
	struct inode *inode = de->d_inode;
	de->d_parent = pdent;
	ia->ia_valid |= ATTR_CTIME;
	ia->ia_ctime = current_time(inode);
	if (inode->i_op && inode->i_op->setattr) {
		int e = inode->i_op->setattr(&nop_mnt_idmap, de, ia);
		if (e) return e;
	} else {
		setattr_copy(&nop_mnt_idmap, inode, ia);       /* fallback (pl. olyan FS, ahol nincs setattr) */
		mark_inode_dirty(inode);
	}
	uk_flush_meta(inode, dir);                         /* az inode (mode/uid/gid) + a szülő perzisztálása */
	return 0;
}

/* ukfs_stat: a VALÓDI inode-attribútumok kiolvasása (a bridge stat-jához — különben a chmod/chown
 * nem látszana, mert a uk_fill_stat hardkódolt 0755/root-ot adott). */
int ukfs_stat(const char *name, unsigned int *mode, unsigned int *uid, unsigned int *gid, long *size, unsigned long *ino, long *mtime, long *atime, unsigned int *nlink, unsigned long *rdev, unsigned long *blocks)
{
	struct dentry *d = (!name || !*name) ? g_api_root : api_lookup(name);
	if (!d || !d->d_inode) return -1;
	struct inode *in = d->d_inode;
	if (mode) *mode = in->i_mode;
	if (uid)  *uid  = in->i_uid.val;
	if (gid)  *gid  = in->i_gid.val;
	if (size) *size = (long)in->i_size;
	if (ino)  *ino  = in->i_ino;
	if (mtime) *mtime = (long)in->i_mtime.tv_sec;
	if (atime) *atime = (long)in->i_atime.tv_sec;
	if (nlink) *nlink = in->i_nlink;       /* a VALÓDI link-szám (hardlinknél 2+) */
	if (rdev)  *rdev  = in->i_rdev;        /* kernel dev_t = (major<<20)|minor (device-node-okhoz) */
	if (blocks) *blocks = (unsigned long)in->i_blocks;  /* VALÓDI foglalt 512B-szektorok (sparse: < size/512 → du kicsi) */
	return 0;
}

/* simple_get_link: a fast-symlink (ext4_fast_symlink_inode_operations) get_link-je — a cél a
 * `inode->i_link`-ben van (az ext4_iget az inline i_block-ra mutatja). A korábbi 0-stub miatt a
 * readlink NULL-t kapott. (A slow symlink az ext4_get_link-et használja, az adatblokkot olvasva.) */
const char *simple_get_link(struct dentry *dentry, struct inode *inode, struct delayed_call *done)
{ (void)dentry; (void)done; return inode->i_link; }
/* inode_set_cached_link: az ext4_iget ezzel mutatja a fast-symlink i_link-jét az inline i_data-ra */
void inode_set_cached_link(struct inode *inode, char *link, int linklen)
{ inode->i_link = link; inode->i_linklen = linklen; }

/* ===== ukfs_symlink: szimbolikus link létrehozása a VALÓDI driver i_op->symlink-jével ===== */
long ukfs_symlink(const char *target, const char *linkpath)
{
	if (!g_api_root || !target) return -1;
	struct inode *dir = 0; struct dentry *pdent = 0; const char *leaf = 0;
	struct dentry *ex = api_walk(linkpath, &dir, &pdent, &leaf);
	if (!dir || !leaf) return -2;
	if (ex && ex->d_inode) return -17;                 /* már létezik (-EEXIST) */
	if (!dir->i_op || !dir->i_op->symlink) return -38; /* a FS nem támogat symlinket (-ENOSYS) */
	struct dentry *nde = calloc(1, sizeof(*nde));
	if (!nde) return -12;
	nde->d_sb = dir->i_sb; nde->d_parent = pdent;
	nde->d_name.name = (const unsigned char *)strdup(leaf);
	nde->d_name.len = strlen(leaf);
	int e = dir->i_op->symlink(&nop_mnt_idmap, dir, nde, target);
	if (e) return e;
	struct inode *inode = nde->d_inode ? nde->d_inode : 0;
	if (!inode) { struct dentry *lr = dir->i_op->lookup(dir, nde, 0); inode = lr ? lr->d_inode : 0; }
	if (inode) uk_flush_meta(inode, dir); else uk_flush_meta(dir, 0);
	return 0;
}

/* ===== ukfs_link: hard link (i_op->link) — ugyanaz az inode, i_nlink++ ===== */
long ukfs_link(const char *oldpath, const char *newpath)
{
	if (!g_api_root) return -1;
	struct dentry *ode = api_walk(oldpath, 0, 0, 0);   /* a meglévő (cél) inode dentryje */
	if (!ode || !ode->d_inode) return -2;              /* nincs ilyen forrás (-ENOENT) */
	if ((ode->d_inode->i_mode & 0170000) == 0040000) return -1;  /* könyvtárra nem (-EPERM) */
	struct inode *dir = 0; struct dentry *pdent = 0; const char *leaf = 0;
	struct dentry *ex = api_walk(newpath, &dir, &pdent, &leaf);
	if (!dir || !leaf) return -2;
	if (ex && ex->d_inode) return -17;                 /* a cél már létezik (-EEXIST) */
	if (!dir->i_op || !dir->i_op->link) return -38;
	struct dentry *nde = calloc(1, sizeof(*nde));
	if (!nde) return -12;
	nde->d_sb = dir->i_sb; nde->d_parent = pdent;
	nde->d_name.name = (const unsigned char *)strdup(leaf);
	nde->d_name.len = strlen(leaf);
	int e = dir->i_op->link(ode, dir, nde);            /* ext4_link: add_entry + inc_nlink + ihold */
	if (e) return e;
	uk_flush_meta(ode->d_inode, dir);                  /* az inode (nlink++) ÉS az új szülő-dir */
	return 0;
}

/* ===== ukfs_readlink: a symlink célja (i_op->get_link); a bájtszámot adja vissza, vagy <0 ===== */
long ukfs_readlink(const char *path, char *buf, size_t bufsz)
{
	struct dentry *de = api_lookup(path);
	if (!de || !de->d_inode) return -2;
	struct inode *inode = de->d_inode;
	if ((inode->i_mode & 0170000) != 0120000) return -22;  /* nem symlink (-EINVAL) */
	if (!inode->i_op || !inode->i_op->get_link) return -22;
	struct delayed_call done; memset(&done, 0, sizeof(done));
	const char *link = inode->i_op->get_link(de, inode, &done);
	if (!link || IS_ERR(link)) return link ? PTR_ERR(link) : -22;
	size_t len = strlen(link); if (len > bufsz) len = bufsz;
	memcpy(buf, link, len);
	return (long)len;
}

/* ===== FIEMAP-mag (a kernel fs/ioctl.c-jének megfelelői; eddig no-op stubok az ext4_stubs.c-ben) ===== */
/* fiemap_prep: a fiemap-kérés előkészítése/validálása (a driver fiemap-je hívja elsőként) */
int fiemap_prep(struct inode *inode, struct fiemap_extent_info *fieinfo, u64 start, u64 *len, u32 supported_flags)
{
	(void)supported_flags;
	if (*len == 0) return -22;                /* -EINVAL */
	u64 maxbytes = (inode->i_sb && inode->i_sb->s_maxbytes) ? inode->i_sb->s_maxbytes : ((u64)1 << 60);
	if (start >= maxbytes) return -27;        /* -EFBIG */
	if (*len > maxbytes - start) *len = maxbytes - start;
	return 0;
}
/* fiemap_fill_next_extent: egy extent hozzáadása a kimenethez (a driver hívja extentenként).
 * Visszaad: 0=siker, 1=a tömb megtelt v. ez az utolsó extent, <0=hiba. (A kernel copy_to_user
 * helyett itt sima memcpy — közös címtér.) */
int fiemap_fill_next_extent(struct fiemap_extent_info *fieinfo, u64 logical, u64 phys, u64 len, u32 flags)
{
	struct fiemap_extent extent;
	if (fieinfo->fi_extents_max == 0) {       /* csak számolunk (fm_extent_count==0) */
		fieinfo->fi_extents_mapped++;
		return (flags & FIEMAP_EXTENT_LAST) ? 1 : 0;
	}
	if (fieinfo->fi_extents_mapped >= fieinfo->fi_extents_max) return 1;   /* megtelt */
	memset(&extent, 0, sizeof(extent));
	extent.fe_logical = logical; extent.fe_physical = phys; extent.fe_length = len; extent.fe_flags = flags;
	memcpy(fieinfo->fi_extents_start + fieinfo->fi_extents_mapped, &extent, sizeof(extent));
	fieinfo->fi_extents_mapped++;
	if (fieinfo->fi_extents_mapped == fieinfo->fi_extents_max) return 1;
	return 0;
}

/* inode_set_bytes / inode_get_bytes: a kernel fs/stat.c-megfelelői (eddig no-op stub az
 * ntfs3_stubs.c-ben → az ntfs3 i_blocks-a MINDIG 0 maradt, így a `du`/SEEK_HOLE/FIEMAP rossz volt).
 * Az ntfs3 ezzel állítja az i_blocks-ot olvasáskor (alloc_size>>9) és íráskor/allokáláskor. */
void inode_set_bytes(struct inode *inode, loff_t bytes)
{ if (inode) { inode->i_blocks = bytes >> 9; inode->i_bytes = (unsigned short)(bytes & 511); } }
loff_t inode_get_bytes(struct inode *inode)
{ return inode ? (((loff_t)inode->i_blocks << 9) + inode->i_bytes) : 0; }

/* iomap_fiemap: a fájl extent-térképe — a kernel iomap-iterátora HELYETT a blokk-allokációból
 * (a_ops->bmap) enumerálunk (driver-független, mint a SEEK_HOLE). Az ext4_fiemap ezt hívja
 * (&ext4_iomap_report_ops-szal, amit IGNORÁLUNK). Eddig no-op stub volt → 0 extent. A szomszédos,
 * fizikailag folytonos blokkokat egy extentté vonjuk; az utolsót FIEMAP_EXTENT_LAST-tel jelöljük. */
int iomap_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo, u64 start, u64 len, const struct iomap_ops *ops)
{
	(void)ops;
	struct address_space *m = inode->i_mapping;
	loff_t size = inode->i_size;
	if (!size || start >= (u64)size) return 0;
	u64 end = start + len; if (end > (u64)size) end = (u64)size;
	/* nincs bmap VAGY rezidens (ntfs3 kis fájl): az egész tartomány EGY adat-extent */
	if (!m || !m->a_ops || !m->a_ops->bmap || inode->i_blocks == 0) {
		fiemap_fill_next_extent(fieinfo, start, 0, end - start, FIEMAP_EXTENT_LAST | FIEMAP_EXTENT_DATA_INLINE | FIEMAP_EXTENT_MERGED);
		return 0;
	}
	unsigned bs = inode->i_sb->s_blocksize ? inode->i_sb->s_blocksize : g_blocksize;
	u64 cur_log = 0, cur_phys = 0, cur_len = 0; int have_cur = 0;   /* épülő folytonos futás */
	u64 p_log = 0, p_phys = 0, p_len = 0; int have_p = 0;           /* kész extent, kiírásra várva (a LAST-hoz) */
	int ret = 0;
	u64 pos = start;
	while (pos < end && !ret) {
		sector_t fblock = pos / bs;
		u64 blk = (u64)fblock * bs;
		sector_t phys = m->a_ops->bmap(m, fblock);
		if (phys) {
			u64 pbyte = (u64)phys * bs;
			if (have_cur && pbyte == cur_phys + cur_len && blk == cur_log + cur_len) {
				cur_len += bs;                          /* folytonos -> bővítjük */
			} else {
				if (have_cur) { if (have_p) ret = fiemap_fill_next_extent(fieinfo, p_log, p_phys, p_len, FIEMAP_EXTENT_MERGED); p_log=cur_log; p_phys=cur_phys; p_len=cur_len; have_p=1; }
				cur_log = blk; cur_phys = pbyte; cur_len = bs; have_cur = 1;
			}
		} else if (have_cur) {                              /* lyuk: lezárja a futást */
			if (have_p) ret = fiemap_fill_next_extent(fieinfo, p_log, p_phys, p_len, FIEMAP_EXTENT_MERGED);
			p_log=cur_log; p_phys=cur_phys; p_len=cur_len; have_p=1; have_cur=0;
		}
		pos = blk + bs;
	}
	if (have_cur && !ret) { if (have_p) ret = fiemap_fill_next_extent(fieinfo, p_log, p_phys, p_len, FIEMAP_EXTENT_MERGED); p_log=cur_log; p_phys=cur_phys; p_len=cur_len; have_p=1; }
	if (have_p && !ret) fiemap_fill_next_extent(fieinfo, p_log, p_phys, p_len, FIEMAP_EXTENT_LAST | FIEMAP_EXTENT_MERGED);
	return 0;
}

/* ===== ukfs_getflags / setflags: inode-flag-ek (chattr/lsattr) a driver fileattr_get/set-jén át ===== */
long ukfs_getflags(const char *name, unsigned int *flags)
{
	struct dentry *d = api_lookup(name);
	if (!d || !d->d_inode) return -2;
	struct inode *in = d->d_inode;
	if (!in->i_op || !in->i_op->fileattr_get) return -95;     /* -EOPNOTSUPP (vfat/exfat) */
	struct file_kattr fa; memset(&fa, 0, sizeof(fa));
	int e = in->i_op->fileattr_get(d, &fa);
	if (e) return e;
	if (flags) *flags = fa.flags;
	return 0;
}
long ukfs_setflags(const char *name, unsigned int flags)
{
	struct dentry *d = api_lookup(name);
	if (!d || !d->d_inode) return -2;
	struct inode *in = d->d_inode;
	if (!in->i_op || !in->i_op->fileattr_set) return -95;
	struct file_kattr old; memset(&old, 0, sizeof(old));      /* a projid megőrzéséhez */
	if (in->i_op->fileattr_get) in->i_op->fileattr_get(d, &old);
	struct file_kattr fa; memset(&fa, 0, sizeof(fa));
	fa.flags = flags; fa.flags_valid = true; fa.fsx_projid = old.fsx_projid;
	int e = in->i_op->fileattr_set(&nop_mnt_idmap, d, &fa);   /* ext4_fileattr_set: csak fa->flags+projid */
	if (e) return e;
	uk_flush_meta(in, 0);
	return 0;
}

/* ===== ukfs_fiemap: a fájl extent-térképe (filefrag) a driver i_op->fiemap-jén át. Az `arg` a
 * felhasználói `struct fiemap *` (UAPI) — közvetlenül feltöltjük (közös címtér). 0=siker, <0=hiba. */
long ukfs_fiemap(const char *name, void *arg)
{
	struct dentry *d = api_lookup(name);
	if (!d || !d->d_inode) return -2;
	struct inode *in = d->d_inode;
	if (!in->i_op || !in->i_op->fiemap) return -95;
	struct fiemap *fm = arg;
	struct fiemap_extent_info fieinfo;
	fieinfo.fi_flags = fm->fm_flags;
	fieinfo.fi_extents_max = fm->fm_extent_count;
	fieinfo.fi_extents_mapped = 0;
	fieinfo.fi_extents_start = fm->fm_extents;
	int e = in->i_op->fiemap(in, &fieinfo, fm->fm_start, fm->fm_length);
	if (e) return e;
	fm->fm_mapped_extents = fieinfo.fi_extents_mapped;
	return 0;
}

/* ===== xattr (kiterjesztett attribútumok) — a VALÓDI driver xattr-handlerein át =====
 * A kernel a setxattr/getxattr-t a sb->s_xattr handler-tömbön keresztül diszpécseli: a név
 * prefixe (user./trusted./security./system.) dönti el a handlert, majd a prefix-et levágva hívja
 * a handler ->get/->set-jét. Ez a uk_xattr_resolve a kernel xattr_resolve_name-jét másolja. */
static const struct xattr_handler *uk_xattr_resolve(struct inode *inode, const char **name)
{
	const struct xattr_handler * const *handlers = inode->i_sb ? inode->i_sb->s_xattr : 0;
	if (!handlers || !name || !*name) return 0;
	for (; *handlers; handlers++) {
		const struct xattr_handler *h = *handlers;
		const char *pfx = h->prefix ? h->prefix : h->name;
		if (!pfx) continue;
		size_t pl = strlen(pfx);
		if (strncmp(*name, pfx, pl) != 0) continue;
		if (h->prefix) {                 /* prefix-handler (pl. "user."): a prefix után KELL név */
			if ((*name)[pl] == 0) continue;
			*name += pl;                 /* a handler a prefix NÉLKÜLI nevet kapja */
		} else {                          /* name-handler: PONTOS egyezés */
			if ((*name)[pl] != 0) continue;
		}
		return h;
	}
	return 0;
}

/* a system.posix_acl_access/default speciális xattr-ek a posix-ACL-úton mennek (i_op->set_acl/
 * get_inode_acl + posix_acl<->xattr konverzió), NEM a sima s_xattr-handleren. -1 ha nem ACL-név. */
static int uk_acl_type(const char *name)
{
	if (!strcmp(name, XATTR_NAME_POSIX_ACL_ACCESS))  return ACL_TYPE_ACCESS;
	if (!strcmp(name, XATTR_NAME_POSIX_ACL_DEFAULT)) return ACL_TYPE_DEFAULT;
	return -1;
}

long ukfs_setxattr(const char *path, const char *name, const void *value, size_t size, int flags)
{
	struct dentry *d = api_lookup(path);
	if (!d || !d->d_inode) return -2;          /* -ENOENT */
	struct inode *in = d->d_inode;
	int atype = uk_acl_type(name);
	if (atype >= 0) {                          /* POSIX ACL: i_op->set_acl a posix_acl-objektummal */
		if (!in->i_op || !in->i_op->set_acl) return -95;   /* -EOPNOTSUPP */
		struct posix_acl *acl = posix_acl_from_xattr(0, value, size);
		if (IS_ERR(acl)) return PTR_ERR(acl);
		int e = in->i_op->set_acl(&nop_mnt_idmap, d, acl, atype);
		posix_acl_release(acl);
		if (e) return e;
		uk_flush_meta(in, 0);
		return 0;
	}
	const char *nm = name;
	const struct xattr_handler *h = uk_xattr_resolve(in, &nm);
	if (!h || !h->set) return -95;             /* -EOPNOTSUPP (vfat/exfat: nincs s_xattr) */
	int e = h->set(h, &nop_mnt_idmap, d, in, nm, value, size, flags);
	if (e) return e;
	uk_flush_meta(in, 0);                       /* az xattr-blokk/inline-xattr perzisztálása */
	return 0;
}

long ukfs_getxattr(const char *path, const char *name, void *value, size_t size)
{
	struct dentry *d = api_lookup(path);
	if (!d || !d->d_inode) return -2;
	struct inode *in = d->d_inode;
	int atype = uk_acl_type(name);
	if (atype >= 0) {                          /* POSIX ACL: get_inode_acl (ext4) v. get_acl (ntfs3) -> posix_acl_to_xattr */
		struct posix_acl *acl;
		if (in->i_op && in->i_op->get_inode_acl) acl = in->i_op->get_inode_acl(in, atype, false);
		else if (in->i_op && in->i_op->get_acl)  acl = in->i_op->get_acl(&nop_mnt_idmap, d, atype);
		else return -95;
		if (IS_ERR(acl)) return PTR_ERR(acl);
		if (!acl) return -61;                  /* -ENODATA (nincs ilyen ACL) */
		long r = posix_acl_to_xattr(0, acl, value, size);
		posix_acl_release(acl);
		return r;
	}
	const char *nm = name;
	const struct xattr_handler *h = uk_xattr_resolve(in, &nm);
	if (!h || !h->get) return -95;
	return h->get(h, d, in, nm, value, size);   /* a méretet adja vissza, vagy <0 (-ENODATA stb.) */
}

long ukfs_listxattr(const char *path, char *list, size_t size)
{
	struct dentry *d = api_lookup(path);
	if (!d || !d->d_inode) return -2;
	struct inode *in = d->d_inode;
	if (!in->i_op || !in->i_op->listxattr) return 0;   /* nincs xattr-támogatás -> üres lista */
	return in->i_op->listxattr(d, list, size);
}

long ukfs_removexattr(const char *path, const char *name)
{
	/* a kernelben a removexattr = a handler ->set-je NULL értékkel + XATTR_REPLACE flaggel */
	struct dentry *d = api_lookup(path);
	if (!d || !d->d_inode) return -2;
	struct inode *in = d->d_inode;
	int atype = uk_acl_type(name);
	if (atype >= 0) {                          /* POSIX ACL törlése: set_acl NULL acl-lel */
		if (!in->i_op || !in->i_op->set_acl) return -95;
		int e = in->i_op->set_acl(&nop_mnt_idmap, d, NULL, atype);
		if (e) return e;
		uk_flush_meta(in, 0);
		return 0;
	}
	const char *nm = name;
	const struct xattr_handler *h = uk_xattr_resolve(in, &nm);
	if (!h || !h->set) return -95;
	int e = h->set(h, &nop_mnt_idmap, d, in, nm, NULL, 0, XATTR_REPLACE);
	if (e) return e;
	uk_flush_meta(in, 0);
	return 0;
}

/* ===== ukfs_chmod: jogosultság-bitek módosítása (i_op->setattr, ATTR_MODE) ===== */
long ukfs_chmod(const char *name, unsigned int mode)
{
	struct dentry *de = api_lookup(name);
	if (!de || !de->d_inode) return -2;
	struct iattr ia; memset(&ia, 0, sizeof(ia));
	ia.ia_valid = ATTR_MODE;
	ia.ia_mode = (de->d_inode->i_mode & 0170000) | (mode & 07777);  /* a típus-biteket megőrizzük */
	return uk_setattr(name, &ia);
}

/* ===== ukfs_chown: tulajdonos/csoport módosítása (i_op->setattr, ATTR_UID/GID) ===== */
long ukfs_chown(const char *name, unsigned int uid, unsigned int gid)
{
	struct iattr ia; memset(&ia, 0, sizeof(ia));
	if (uid != (unsigned)-1) { ia.ia_valid |= ATTR_UID; ia.ia_uid = (kuid_t){ .val = uid }; }
	if (gid != (unsigned)-1) { ia.ia_valid |= ATTR_GID; ia.ia_gid = (kgid_t){ .val = gid }; }
	if (!(ia.ia_valid & (ATTR_UID | ATTR_GID))) return 0;
	return uk_setattr(name, &ia);
}

/* ===== ukfs_truncate: fájl-méret állítása (i_op->setattr, ATTR_SIZE) — zsugorítás blokk-
 * felszabadítással (ext4_truncate), növelés zero-fill-lel (ritka/sparse) ===== */
long ukfs_truncate(const char *name, long long size)
{
	struct dentry *d = api_lookup(name);
	if (!d || !d->d_inode) return -2;
	if ((d->d_inode->i_mode & 0170000) == 0040000) return -21;  /* könyvtár (-EISDIR) */
	struct iattr ia; memset(&ia, 0, sizeof(ia));
	ia.ia_valid = ATTR_SIZE | ATTR_MTIME;
	ia.ia_size = size;
	ia.ia_mtime = current_time(d->d_inode);
	return uk_setattr(name, &ia);
}

/* ===== ukfs_utime: hozzáférési/módosítási idő állítása (i_op->setattr, ATTR_ATIME/MTIME) =====
 * ansec/mnsec == -1 -> az adott időt NEM állítjuk (UTIME_OMIT); egyébként a (sec,nsec) érték. */
long ukfs_utime(const char *name, long asec, long ansec, long msec, long mnsec)
{
	struct dentry *d = api_lookup(name);
	if (!d || !d->d_inode) return -2;
	struct iattr ia; memset(&ia, 0, sizeof(ia));
	if (ansec != -1) { ia.ia_valid |= ATTR_ATIME | ATTR_ATIME_SET; ia.ia_atime.tv_sec = asec; ia.ia_atime.tv_nsec = ansec; }
	if (mnsec != -1) { ia.ia_valid |= ATTR_MTIME | ATTR_MTIME_SET; ia.ia_mtime.tv_sec = msec; ia.ia_mtime.tv_nsec = mnsec; }
	if (!(ia.ia_valid & (ATTR_ATIME | ATTR_MTIME))) return 0;   /* mindkettő OMIT */
	return uk_setattr(name, &ia);
}

/* ===== ukfs_write_file: VALÓDI írás a driveren át (create + a_ops->write_begin/end) ===== */
long ukfs_write_file(const char *name, const char *data, size_t len)
{
	if (!g_api_root) return -1;
	struct inode *dir = 0; struct dentry *pdent = 0; const char *leaf = 0;
	struct dentry *de = api_walk(name, &dir, &pdent, &leaf);
	if (!dir || !leaf) return -1;          /* rossz út (köztes komponens nem könyvtár) */
	struct inode *inode = (de && de->d_inode) ? de->d_inode : 0;
	if (!inode) {                         /* nem létezik -> létrehozás a valódi driverrel */
		if (!dir->i_op || !dir->i_op->create) return -2;
		struct dentry *nde = calloc(1, sizeof(*nde));
		nde->d_sb = dir->i_sb; nde->d_parent = pdent;
		nde->d_name.name = (const unsigned char *)strdup(leaf);
		nde->d_name.len = strlen(leaf);
		int e = dir->i_op->create(&nop_mnt_idmap, dir, nde, 0100644, true);
		if (e) return e;
		de = nde;
		inode = nde->d_inode;
		if (!inode) { struct dentry *r = dir->i_op->lookup(dir, nde, 0); inode = r ? r->d_inode : 0; }
	}
	if (!inode) return -3;
	/* írásra-nyitás: a driver f_op->open hookja (ext4: ext4_file_open) FMODE_WRITE-nál
	 * attachelja a jbd2-inode-ot (EXT4_I->jinode) — különben az ordered-mode data-tracking
	 * (jbd2_journal_file_inode) NULL jinode-on elszáll. Ez a kernel valódi open-for-write útja. */
	if (getenv("UK_FS_DEBUG")) fprintf(stderr, "[vfs] open-for-write: i_fop=%p open=%p\n", (void*)inode->i_fop, inode->i_fop?(void*)inode->i_fop->open:0);
	if (inode->i_fop && inode->i_fop->open) {
		struct file of; memset(&of, 0, sizeof(of));
		of.f_inode = inode; of.f_mapping = inode->i_mapping; of.f_op = inode->i_fop;
		of.f_path.dentry = de; of.f_mode = FMODE_WRITE | FMODE_READ;
		int oe = inode->i_fop->open(inode, &of);
		if (getenv("UK_FS_DEBUG")) fprintf(stderr, "[vfs] open ret=%d\n", oe);
	}
	struct address_space *m = inode->i_mapping;
	if (getenv("UK_FS_DEBUG")) fprintf(stderr, "[vfs] write: inode=%p ino=%lu size=%lld a_ops=%p wb=%p\n", (void*)inode, inode->i_ino, (long long)inode->i_size, (void*)m->a_ops, m->a_ops?(void*)m->a_ops->write_begin:0);
	if (m->a_ops && m->a_ops->write_begin && m->a_ops->write_end) {
		/* vfat/exfat: a_ops->write_begin/write_end (folio a blokk-backenden) */
		/* FELÜLÍRÁS: a létező fájlt előbb 0-ra vágjuk (a régi fürtök felszabadítása), hogy az
		 * írás TISZTÁN, friss fájlként allokáljon. Különben egy kis létező fájl nagyobbra
		 * felülírásakor a méret nőne, de a FAT-fürtlánc nem bővülne rendesen (fsck "cluster
		 * chain length" + adatvesztés). A bridge MINDIG a teljes tartalmat adja (len) → biztonságos. */
		if (inode->i_size > 0 && inode->i_op && inode->i_op->setattr) {
			struct dentry td0; memset(&td0, 0, sizeof(td0)); td0.d_inode = inode; td0.d_sb = inode->i_sb; td0.d_parent = g_api_root;
			struct iattr ia0; memset(&ia0, 0, sizeof(ia0)); ia0.ia_valid = ATTR_SIZE; ia0.ia_size = 0;
			inode->i_op->setattr(&nop_mnt_idmap, &td0, &ia0);
		}
		loff_t pos = 0; size_t rem = len;
		while (rem) {
			unsigned chunk = rem > 4096 ? 4096 : rem;
			struct folio *fo = 0; void *fsdata = 0;
			int e = m->a_ops->write_begin(0, m, pos, chunk, &fo, &fsdata);
			if (e || !fo) return -5;
			char *fa = folio_address(fo);
			memcpy(fa + (pos & 4095), data + pos, chunk);
			m->a_ops->write_end(0, m, pos, chunk, chunk, fo, fsdata);
			pos += chunk; rem -= chunk;
		}
	} else if (inode->i_fop && inode->i_fop->write_iter) {
		/* ntfs3: nincs a_ops->write_begin, a f_op->write_iter -> iomap_file_buffered_write úton ír */
		struct file f; memset(&f, 0, sizeof(f));
		f.f_inode = inode; f.f_mapping = m; f.f_op = inode->i_fop; f.f_path.dentry = de;
		struct kiocb iocb; memset(&iocb, 0, sizeof(iocb)); iocb.ki_filp = &f; iocb.ki_pos = 0;
		struct iov_iter iter; memset(&iter, 0, sizeof(iter)); iter.count = len; iter.uk_data = data; iter.data_source = 0;
		ssize_t w = inode->i_fop->write_iter(&iocb, &iter);
		if (getenv("UK_FS_DEBUG")) fprintf(stderr, "[vfs] ntfs write_iter = %zd (size=%lld)\n", w, (long long)inode->i_size);
		if (w < 0) return w;
	} else return -4;
	/* a temp-fd a fájl TELJES tartalmát adja (len); ha a régi fájl nagyobb volt, len-re vágjuk
	 * (az ext4_truncate felszabadítja a felesleget) — felülírás/zsugorítás pontos méretéhez. */
	if (len > 0 && (loff_t)len < inode->i_size && inode->i_op && inode->i_op->setattr) {
		struct dentry td; memset(&td, 0, sizeof(td)); td.d_inode = inode; td.d_sb = inode->i_sb; td.d_parent = g_api_root;
		struct iattr ia; memset(&ia, 0, sizeof(ia)); ia.ia_valid = ATTR_SIZE; ia.ia_size = (loff_t)len;
		inode->i_op->setattr(&nop_mnt_idmap, &td, &ia);
	}
	/* metaadat visszaírása: a fájl ÉS a könyvtár MFT-rekordja (a dir-index a fájl nevét
	 * tartalmazza) + sync_fs (bitmapek, MFT-mirror, sync_blockdev) — különben a fájl nem
	 * látszik újra-mountkor (az adat már perzisztált az iomap-úton). */
	uk_flush_meta(inode, dir);   /* a fájl + a szülő-könyvtár metaadatának perzisztálása */
	return len;
}

/* ===== ukfs_write_file_at: OFFSET-tudatos írás (sparse fájlokhoz) =====
 * A [offset, offset+len) tartományt írja, a fájl elejének/lyukainak ÉRINTETLENÜL hagyásával.
 * NEM vág 0-ra (megőrzi a többi extentet), NEM állít méretet. Csak az írott blokkokat foglalja
 * (write_begin create=1 a megadott pos-on) → a kihagyott tartomány LYUK marad a lemezen.
 * Létrehozza a fájlt ha nincs. 0=siker, <0=hiba. */
long ukfs_write_file_at(const char *name, const char *data, size_t len, long long offset)
{
	if (!g_api_root) return -1;
	struct inode *dir = 0; struct dentry *pdent = 0; const char *leaf = 0;
	struct dentry *de = api_walk(name, &dir, &pdent, &leaf);
	if (!dir || !leaf) return -1;
	struct inode *inode = (de && de->d_inode) ? de->d_inode : 0;
	if (!inode) {                         /* nem létezik -> létrehozás a valódi driverrel */
		if (!dir->i_op || !dir->i_op->create) return -2;
		struct dentry *nde = calloc(1, sizeof(*nde));
		nde->d_sb = dir->i_sb; nde->d_parent = pdent;
		nde->d_name.name = (const unsigned char *)strdup(leaf);
		nde->d_name.len = strlen(leaf);
		int e = dir->i_op->create(&nop_mnt_idmap, dir, nde, 0100644, true);
		if (e) return e;
		de = nde; inode = nde->d_inode;
		if (!inode) { struct dentry *r = dir->i_op->lookup(dir, nde, 0); inode = r ? r->d_inode : 0; }
	}
	if (!inode) return -3;
	if (inode->i_fop && inode->i_fop->open) {   /* jbd2-inode attach (mint ukfs_write_file) */
		struct file of; memset(&of, 0, sizeof(of));
		of.f_inode = inode; of.f_mapping = inode->i_mapping; of.f_op = inode->i_fop;
		of.f_path.dentry = de; of.f_mode = FMODE_WRITE | FMODE_READ;
		inode->i_fop->open(inode, &of);
	}
	struct address_space *m = inode->i_mapping;
	if (m->a_ops && m->a_ops->write_begin && m->a_ops->write_end) {
		loff_t pos = offset; size_t rem = len; const char *src = data;
		while (rem) {
			/* a folio-határig (4096) írunk egy lépésben — a write_begin a kezdő-blokkot foglalja */
			unsigned within = pos & 4095;
			unsigned chunk = 4096 - within; if (chunk > rem) chunk = rem;
			struct folio *fo = 0; void *fsdata = 0;
			int e = m->a_ops->write_begin(0, m, pos, chunk, &fo, &fsdata);
			if (e || !fo) return -5;
			char *fa = folio_address(fo);
			memcpy(fa + (pos & 4095), src, chunk);
			m->a_ops->write_end(0, m, pos, chunk, chunk, fo, fsdata);
			pos += chunk; rem -= chunk; src += chunk;
		}
	} else if (inode->i_fop && inode->i_fop->write_iter) {
		struct file f; memset(&f, 0, sizeof(f));
		f.f_inode = inode; f.f_mapping = m; f.f_op = inode->i_fop; f.f_path.dentry = de;
		struct kiocb iocb; memset(&iocb, 0, sizeof(iocb)); iocb.ki_filp = &f; iocb.ki_pos = offset;
		struct iov_iter iter; memset(&iter, 0, sizeof(iter)); iter.count = len; iter.uk_data = data; iter.data_source = 0;
		ssize_t w = inode->i_fop->write_iter(&iocb, &iter);
		if (w < 0) return w;
	} else return -4;
	/* Keep the dir-entry/inode metadata current in the buffer cache (size grows as
	 * the file is written), but DON'T sync to the block device here: sync_blockdev
	 * writes the whole dirty-buffer list, so doing it per write() is O(n^2) for a
	 * large file (wget/dd writing in small chunks). The data pages + metadata stay
	 * dirty in cache; the redirect sends SYNC on close, which flushes once (O(n)).
	 * In-session reads/stats are served from the page cache, so they stay correct. */
	uk_meta_inode_only(inode, dir);
	return len;
}

/* Flush a written file to the block device — the deferred counterpart of the
 * per-write fast path above, invoked via the redirect's SYNC on close. */
long ukfs_sync_path(const char *name)
{
	if (!g_api_root) return -1;
	struct dentry *d = (!name || !*name) ? g_api_root : api_lookup(name);
	struct inode *in = (d && d->d_inode) ? d->d_inode : 0;
	if (!in) {                       /* file gone (e.g. renamed away): sync the whole FS */
		struct super_block *sb = g_api_root->d_sb;
		if (sb && sb->s_op && sb->s_op->sync_fs) sb->s_op->sync_fs(sb, 1);
		if (sb) sync_blockdev(sb->s_bdev);
		return 0;
	}
	uk_flush_meta(in, 0);            /* write_inode + sync_fs + sync_blockdev */
	return 0;
}

/* ntfs3 metaadat-olvasás: __getblk + __filemap_get_folio a blokk-backendről */
struct buffer_head *__getblk(struct block_device *bdev, sector_t block, unsigned size)
{
	(void)bdev;
	struct buffer_head *bh = bh_cache_find(block, size);
	if (bh) { bh->b_count++; return bh; }
	bh = calloc(1, sizeof(*bh));
	if (!bh) return 0;
	bh->b_data = calloc(1, size); bh->b_size = size; bh->b_blocknr = block; bh->b_count = 1;
	if (bh->b_data) bdev_pread(bh->b_data, size, (off_t)block * size);
	bh->b_state = (1UL << BH_Uptodate) | (1UL << BH_Mapped);
	bh_register(bh);
	return bh;
}
/* getblk_unmovable / bdev_getblk (ext4): olvasott, uptodate buffer (a __getblk-en át) */
struct buffer_head *getblk_unmovable(struct block_device *bdev, sector_t block, unsigned size)
{ return __getblk(bdev, block, size); }
/* ===== minimál page-cache: az ext4 buddy-allokátorhoz KELL (ugyanazt a (mapping,index)
 * folio-t kell visszaadni, hogy a buddy-bitmap generálás perzisztáljon, és a folio_put
 * refcount-alapú legyen — különben double-free / elveszett buddy-adat). ===== */
struct uk_fcache { struct address_space *m; pgoff_t idx; struct folio *fo; int ref; struct uk_fcache *next; };
/* g_fcache: a teljes definíció — a fenti fwd-decl tentatív, itt egyesül vele. */
static struct uk_fcache *fcache_find(struct address_space *m, pgoff_t idx)
{ for (struct uk_fcache *e = g_fcache; e; e = e->next) if (e->m == m && e->idx == idx) return e; return 0; }

struct folio *__filemap_get_folio(struct address_space *mapping, pgoff_t index, int fgp, gfp_t gfp)
{
	(void)gfp;
	struct uk_fcache *e = fcache_find(mapping, index);
	if (e) { e->ref++; if (fgp & 0x02) e->fo->flags |= 1UL;   /* FGP_LOCK(0x02) -> PG_locked */ return e->fo; }
	
	struct folio *fo = calloc(1, sizeof(*fo));
	if (!fo) return (struct folio *)ERR_PTR(-2);   /* -ENOENT */
	void *data = calloc(1, 4096);
	fo->_priv = data; fo->mapping = mapping; fo->index = index;
	fo->page._priv = data; fo->page.mapping = mapping; fo->page.index = index;
	if (fgp & 0x02) fo->flags |= 1UL;   /* FGP_LOCK(0x02) -> PG_locked */
	if (mapping && mapping->a_ops && mapping->a_ops->bmap && mapping->host) {
		struct inode *inode = mapping->host;
		unsigned bs = inode->i_sb->s_blocksize ? inode->i_sb->s_blocksize : g_blocksize;
		unsigned per = 4096 / bs; if (!per) per = 1;
		for (unsigned b = 0; b < per; b++) {
			sector_t phys = mapping->a_ops->bmap(mapping, (sector_t)index * per + b);
			if (phys && data) { bdev_pread((char *)data + b * bs, bs, (off_t)phys * bs); fo->flags |= 2UL; }
		}
	}
	e = calloc(1, sizeof(*e));
	if (e) { e->m = mapping; e->idx = index; e->fo = fo; e->ref = 1; e->next = g_fcache; g_fcache = e; }
	return fo;
}
/* filemap_get_folio / filemap_lock_folio: csak lekérdezés (nincs CREAT) — cache-ből */
struct folio *filemap_get_folio(struct address_space *mapping, pgoff_t index)
{ struct uk_fcache *e = fcache_find(mapping, index); if (e) { e->ref++; return e->fo; } return (struct folio *)ERR_PTR(-2); }
struct folio *filemap_lock_folio(struct address_space *mapping, pgoff_t index)
{ struct uk_fcache *e = fcache_find(mapping, index); if (e) { e->ref++; e->fo->flags |= 1UL; return e->fo; } return (struct folio *)ERR_PTR(-2); }

/* ===== ntfs3 valódi bio/folio/kmap réteg (a backendre olvas/ír) =====
 * A shimben a "page" MAGA az adatpuffer (page_address(p)==(void*)p, lásd slab.h). */
#include <linux/blk_types.h>
void *kmap_atomic(struct page *page) { return (void *)page; }
void *kmap_local_page(struct page *page) { return (void *)page; }
void  kunmap_atomic(void *addr) { (void)addr; }
void  kunmap_local(const void *addr) { (void)addr; }
void *kmap_local_folio(struct folio *folio, size_t offset) { return folio ? (char *)folio->_priv + offset : 0; }

struct bio *bio_alloc(struct block_device *bdev, unsigned short nr, blk_opf_t opf, gfp_t gfp)
{
	(void)gfp;
	struct bio *bio = calloc(1, sizeof(*bio));
	if (!bio) return 0;
	bio->bi_bdev = bdev; bio->bi_opf = opf;
	bio->bi_io_vec = calloc(nr ? nr : 1, sizeof(struct bio_vec));
	bio->bi_vcnt = 0;
	return bio;
}
int bio_add_page(struct bio *bio, struct page *page, unsigned int len, unsigned int off)
{
	struct bio_vec *bv = &bio->bi_io_vec[bio->bi_vcnt];
	bv->bv_page = page; bv->bv_len = len; bv->bv_offset = off;
	bio->bi_vcnt++;
	return len;
}
static int uk_bio_io(struct bio *bio)
{
	off_t pos = (off_t)bio->bi_iter.bi_sector << 9;
	int write = (bio->bi_opf & 1);   /* REQ_OP_WRITE=1, READ=0; a flagek nem állítják a 0. bitet */
	for (int i = 0; i < bio->bi_vcnt; i++) {
		struct bio_vec *bv = &bio->bi_io_vec[i];
		void *data = (char *)page_address((struct page *)bv->bv_page) + bv->bv_offset;
		if (write) bdev_pwrite(data, bv->bv_len, pos);
		else       bdev_pread(data, bv->bv_len, pos);
		pos += bv->bv_len;
	}
	bio->bi_status = 0;
	return 0;
}
void submit_bio(struct bio *bio) { uk_bio_io(bio); if (bio->bi_end_io) bio->bi_end_io(bio); }
int  submit_bio_wait(struct bio *bio) { return uk_bio_io(bio); }
void bio_chain(struct bio *bio, struct bio *new) { (void)bio; (void)new; }  /* szinkron: nincs valódi lánc */
void bio_put(struct bio *bio) { if (bio) { free(bio->bi_io_vec); free(bio); } }
struct bio *bio_alloc_bioset(struct block_device *bdev, unsigned short nr, blk_opf_t opf, gfp_t gfp, void *bs)
{ (void)bs; return bio_alloc(bdev, nr, opf, gfp); }

struct folio *folio_alloc(gfp_t gfp, unsigned int order) { (void)gfp; struct folio *fo = calloc(1, sizeof(*fo)); if (fo) { fo->_priv = calloc(1, 4096UL << order); } return fo; }
/* folio_put: ha cache-elt, refcount-- és csak 0-nál szabadít+leláncol; különben sima szabadítás. */
void folio_put(struct folio *folio)
{
	if (!folio) return;
	struct uk_fcache **pp = &g_fcache;
	for (struct uk_fcache *e = g_fcache; e; pp = &e->next, e = e->next) {
		if (e->fo == folio) {
			if (--e->ref > 0) return;          /* még van hivatkozás */
			*pp = e->next; free(e);            /* leláncol */
			free(folio->_priv); free(folio);
			return;
		}
	}
	free(folio->_priv); free(folio);           /* nem cache-elt (write_begin/folio_alloc) */
}
void folio_get(struct folio *folio) { if (!folio) return; for (struct uk_fcache *e = g_fcache; e; e = e->next) if (e->fo == folio) { e->ref++; return; } }
/* folio-flag bitek: 0=PG_locked, 1=PG_uptodate, 2=PG_dirty. Az ext4 buddy-betöltés a
 * folio_test_uptodate-re hagyatkozik (a no-op miatt -EIO-t adott a buddy-cache). */
void folio_mark_uptodate(struct folio *folio) { if (folio) folio->flags |= 2UL; }
void __folio_mark_uptodate(struct folio *folio) { if (folio) folio->flags |= 2UL; }
int folio_test_uptodate(struct folio *folio) { return folio ? (int)((folio->flags >> 1) & 1) : 0; }
int folio_test_dirty(struct folio *folio) { return folio ? (int)((folio->flags >> 2) & 1) : 0; }
void folio_mark_dirty(struct folio *folio) { if (folio) folio->flags |= 4UL; }
size_t folio_size(const void *folio) { (void)folio; return 4096; }
/* folio_pos: a folio fájlbeli offszetje (index * laphossz) — a driver read_folio-ja
 * ezzel számolja a vbo-t; a stub-0 miatt minden olvasás a 0. offszetről indult. */
loff_t folio_pos(struct folio *folio) { return folio ? (loff_t)folio->index * 4096 : 0; }
/* folio_next_pos: a folio UTÁNI fájl-offszet (egy-lapos folio: pos+4096). A stub-0 miatt az
 * ext4_write_begin a len-t 0-ra csonkította → semmit sem írt. */
loff_t folio_next_pos(struct folio *folio) { return folio ? (loff_t)(folio->index + 1) * 4096 : 4096; }
unsigned long folio_nr_pages(const struct folio *folio) { (void)folio; return 1; }
/* folio-lock modell: folio->flags 0. bit = PG_locked (az ext4 BUG_ON-ozik rá) */
#define UK_PG_LOCKED 1UL
void folio_lock(struct folio *folio) { if (folio) folio->flags |= UK_PG_LOCKED; }
void folio_unlock(struct folio *folio) { if (folio) folio->flags &= ~UK_PG_LOCKED; }
int folio_test_locked(struct folio *folio) { return folio ? (int)(folio->flags & UK_PG_LOCKED) : 0; }
int folio_trylock(struct folio *folio) { if (folio) folio->flags |= UK_PG_LOCKED; return 1; }
void folio_zero_range(struct folio *folio, size_t from, size_t len)
{ char *d = folio_address(folio); if (d && from < 4096) { size_t n = (from + len > 4096) ? 4096 - from : len; memset(d + from, 0, n); } }
void folio_zero_segment(struct folio *folio, size_t from, size_t to)
{ if (to > from) folio_zero_range(folio, from, to - from); }

/* ___ratelimit: engedjük át az ntfs3 hibaüzeneteit (diagnosztika) */
int ___ratelimit(struct ratelimit_state *rs, const char *func) { (void)rs; (void)func; return 1; }

/* ntfs3: sb_bread_unmovable = valódi sb_bread (a metaadat-olvasáshoz) */
struct buffer_head *sb_bread_unmovable(struct super_block *sb, sector_t block) { return sb_bread(sb, block); }

/* bdev_nr_bytes: a backend (image/eszköz) tényleges mérete — az ntfs3 ebből számol blocks-ot */
#include <sys/stat.h>
u64 bdev_nr_bytes(struct block_device *bdev) { (void)bdev; if (g_bdev_sock >= 0) { long long sz = bsock_size(); return sz > 0 ? (u64)sz : 0; } struct stat st; if (g_bdev_fd >= 0 && fstat(g_bdev_fd, &st) == 0) { if (st.st_size) return st.st_size; off_t e = lseek(g_bdev_fd, 0, SEEK_END); return e > 0 ? (u64)e : 0; } return 0; }

/* inode_state_read_once: az ntfs3 az I_NEW-t ebből olvassa (iget5_locked állítja) */
unsigned long inode_state_read_once(struct inode *inode) { return inode ? inode->i_state : 0; }

/* ===== bit-keresők (a cluster/MFT-bitmap szkenneléshez; a no-op return-0 végtelen ciklust okozott) ===== */
unsigned long find_next_bit(const unsigned long *addr, unsigned long size, unsigned long offset)
{ for (unsigned long i = offset; i < size; i++) if (addr[i/64] & (1UL << (i%64))) return i; return size; }
unsigned long find_next_zero_bit(const unsigned long *addr, unsigned long size, unsigned long offset)
{ for (unsigned long i = offset; i < size; i++) if (!(addr[i/64] & (1UL << (i%64)))) return i; return size; }
/* little-endian (bájtonkénti) variánsok — az NTFS on-disk bitmap LE */
unsigned long find_next_zero_bit_le(const void *addr, unsigned long size, unsigned long offset)
{ const unsigned char *p = addr; for (unsigned long i = offset; i < size; i++) if (!(p[i/8] & (1u << (i%8)))) return i; return size; }
unsigned long find_next_bit_le(const void *addr, unsigned long size, unsigned long offset)
{ const unsigned char *p = addr; for (unsigned long i = offset; i < size; i++) if (p[i/8] & (1u << (i%8))) return i; return size; }

/* ===== iomap_bmap + read_mapping_page/folio: a driver iomap_begin-jén át olvas a backendről ===== */
#include <linux/iomap.h>
sector_t iomap_bmap(struct address_space *mapping, sector_t block, const struct iomap_ops *ops)
{
	struct inode *inode = mapping->host;
	unsigned bs = inode->i_sb->s_blocksize ? inode->i_sb->s_blocksize : g_blocksize;
	struct iomap iomap; memset(&iomap, 0, sizeof(iomap));
	loff_t pos = (loff_t)block * bs;
	if (!ops || !ops->iomap_begin) return 0;
	if (ops->iomap_begin(inode, pos, bs, 0, &iomap, 0)) return 0;
	/* MAPPED + érvényes addr elég (az ext4 iomap_begin length=0-t adhat 1-blokkos lekérdezésre);
	 * csak HOLE/DELALLOC/NULL_ADDR esetén nincs fizikai blokk. */
	if (iomap.addr == IOMAP_NULL_ADDR || iomap.type == IOMAP_HOLE || iomap.type == IOMAP_DELALLOC) return 0;
	return (sector_t)((iomap.addr + (pos - iomap.offset)) / bs);
}
struct page *read_mapping_page(struct address_space *m, pgoff_t index, struct file *file)
{
	(void)file;
	struct inode *inode = m->host;
	unsigned bs = inode->i_sb->s_blocksize ? inode->i_sb->s_blocksize : g_blocksize;
	char *page = calloc(1, 4096);                  /* a page MAGA az adat (page_address=(void*)p) */
	if (!page) return (struct page *)ERR_PTR(-12);
	unsigned per = 4096 / bs; if (!per) per = 1;
	if (m->a_ops && m->a_ops->bmap)
		for (unsigned b = 0; b < per; b++) {
			sector_t phys = m->a_ops->bmap(m, (sector_t)index * per + b);
			if (phys) bdev_pread(page + b * bs, bs, (off_t)phys * bs);
		}
	return (struct page *)page;
}
struct folio *read_mapping_folio(struct address_space *m, pgoff_t index, struct file *file)
{
	struct folio *fo = calloc(1, sizeof(*fo));
	if (!fo) return (struct folio *)ERR_PTR(-12);
	struct page *p = read_mapping_page(m, index, file);
	fo->_priv = p; fo->mapping = m; fo->index = index;
	fo->page.mapping = m; fo->page.index = index; fo->page._priv = p;
	return fo;
}

/* ===== iomap buffered write (ntfs3 ír ezen át): iov_iter -> iomap_begin(IOMAP_WRITE) -> bdev_pwrite ===== */
#include <linux/uio.h>
size_t copy_from_iter(void *addr, size_t bytes, struct iov_iter *i)
{
	size_t n = bytes > i->count ? i->count : bytes;
	if (i->uk_data) { memcpy(addr, i->uk_data, n); i->uk_data = (const char *)i->uk_data + n; }
	i->count -= n;
	return n;
}
size_t copy_to_iter(const void *addr, size_t bytes, struct iov_iter *i)
{
	size_t n = bytes > i->count ? i->count : bytes;
	if (i->uk_data) { memcpy((void *)i->uk_data, addr, n); i->uk_data = (const char *)i->uk_data + n; }
	i->count -= n;
	return n;
}
size_t copy_page_from_iter_atomic(struct page *page, size_t off, size_t bytes, struct iov_iter *i)
{ return copy_from_iter((char *)page_address(page) + off, bytes, i); }

ssize_t iomap_file_buffered_write(struct kiocb *iocb, struct iov_iter *from,
				  const struct iomap_ops *ops, const struct iomap_write_ops *wops, void *priv)
{
	(void)wops; (void)priv;
	struct inode *inode = iocb->ki_filp->f_inode ? iocb->ki_filp->f_inode : iocb->ki_filp->f_mapping->host;
	if (!ops || !ops->iomap_begin || !inode) return -22;
	loff_t pos = iocb->ki_pos;
	size_t total = from->count, done = 0;
	char *tmp = malloc(total ? total : 1);
	while (done < total) {
		struct iomap iomap; memset(&iomap, 0, sizeof(iomap));
		size_t want = total - done;
		loff_t cpos = pos + done;                       /* a chunk KEZDŐ pozíciója */
		if (ops->iomap_begin(inode, cpos, want, IOMAP_WRITE, &iomap, 0)) break;
		size_t chunk = iomap.length && iomap.length < want ? (size_t)iomap.length : want;
		size_t written = 0;
		if (iomap.type == 5 /*IOMAP_INLINE*/ && iomap.inline_data) {
			/* REZIDENS adat: az MFT-rekord inline-pufferébe másol; az iomap_end másolja
			 * vissza a valódi rezidens DATA-attribútumba (a write_inode perzisztálja). */
			void *dst = (char *)iomap.inline_data + (cpos - iomap.offset);
			written = copy_from_iter(dst, chunk, from);
			done += written;
		} else if (iomap.type == 2 /*IOMAP_MAPPED*/ && iomap.addr != IOMAP_NULL_ADDR && chunk) {
			written = copy_from_iter(tmp, chunk, from);
			off_t doff = iomap.addr + (cpos - iomap.offset);
			bdev_pwrite(tmp, written, doff);
			done += written;
		} else { from->count -= chunk; done += chunk; written = chunk; }   /* hole — átugorjuk */
		/* iomap_end a chunk KEZDŐ pozíciójával + a ténylegesen írt bájtszámmal (a ntfs_iomap_end
		 * INLINE-ágának pos<data_size ellenőrzése különben elbukik és nem ír vissza). */
		if (ops->iomap_end) ops->iomap_end(inode, cpos, chunk, written, IOMAP_WRITE, &iomap);
	}
	free(tmp);
	if (pos + (loff_t)done > inode->i_size) inode->i_size = pos + done;
	iocb->ki_pos = pos + done;
	return done ? (ssize_t)done : -22;
}

/* ===== iomap_read_folio (ntfs3 OLVAS ezen át): a driver iomap_begin(READ)-jén keresztül
 * tölti a folio adatpufferét. Az iomap_file_buffered_write tükre: kezeli a REZIDENS
 * (IOMAP_INLINE, MFT-be ágyazott) ÉS a nem-rezidens (IOMAP_MAPPED, lemez) adatot, a
 * lyukat (HOLE) nullázza — driver-független, nem kerülgeti a problémát. */
void iomap_read_folio(const struct iomap_ops *ops, struct iomap_read_folio_ctx *ctx, void *priv)
{
	(void)priv;
	if (!ctx || !ctx->cur_folio || !ops || !ops->iomap_begin) return;
	struct folio *folio = ctx->cur_folio;
	struct inode *inode = folio->mapping ? folio->mapping->host : 0;
	char *dst = folio_address(folio);
	if (!inode || !dst) { folio_unlock(folio); return; }
	loff_t fpos = folio_pos(folio);
	size_t fsize = 4096;
	if (fpos + (loff_t)fsize > inode->i_size && inode->i_size > fpos) fsize = inode->i_size - fpos;
	size_t done = 0;
	while (done < fsize) {
		struct iomap iomap; memset(&iomap, 0, sizeof(iomap));
		loff_t pos = fpos + done;
		size_t want = fsize - done;
		if (ops->iomap_begin(inode, pos, want, IOMAP_READ, &iomap, 0)) break;
		size_t chunk = iomap.length && iomap.length < want ? (size_t)iomap.length : want;
		if (!chunk) break;
		if (iomap.type == IOMAP_INLINE && iomap.inline_data) {
			memcpy(dst + done, (char *)iomap.inline_data + (pos - iomap.offset), chunk);
		} else if (iomap.type == IOMAP_MAPPED && iomap.addr != IOMAP_NULL_ADDR) {
			bdev_pread(dst + done, chunk, iomap.addr + (pos - iomap.offset));
		} else {
			memset(dst + done, 0, chunk);   /* HOLE/unwritten */
		}
		if (ops->iomap_end) ops->iomap_end(inode, pos, chunk, chunk, IOMAP_READ, &iomap);
		done += chunk;
	}
	folio_mark_uptodate(folio);
	folio_unlock(folio);
}

/* inode_init_owner: beállítja az új inode mode-ját (+uid/gid). A no-op stub miatt az
 * ntfs_create_inode i_mode-ja 0 maradt → a fájl special inode lett (nincs a_ops/write_iter)! */
void inode_init_owner(struct mnt_idmap *idmap, struct inode *inode, const struct inode *dir, umode_t mode)
{ (void)idmap; (void)dir; if (inode) inode->i_mode = mode; }

/* folio_contains: a folio tartalmazza-e a pgoff_t index-et (egy-lapos: index==folio->index).
 * A no-op-0 miatt az ext4 buddy-cache külön folio-t kért bitmap+buddy-nak (közös page helyett)
 * → dupla init_cache → BUG_ON. */
int folio_contains(struct folio *folio, unsigned long index)
{ return folio ? (index == folio->index) : 0; }

/* uk_dquot_alloc_block: i_blocks frissítése (512-bájtos szektorokban) — nr fs-blokk.
 * A no-op miatt i_blocks=0 maradt, a round-trip-olvasás rossz lett. */
int uk_dquot_alloc_block(struct inode *inode, long long nr)
{ if (inode) inode->i_blocks += (blkcnt_t)nr << (inode->i_blkbits - 9); return 0; }
/* uk_dquot_free_block: i_blocks CSÖKKENTÉSE blokk-felszabadításkor (truncate/unlink) — a no-op
 * miatt az i_blocks túl magas maradt (fsck "i_blocks is N, should be M"). */
void uk_dquot_free_block(struct inode *inode, long long nr)
{ if (inode) { blkcnt_t d = (blkcnt_t)nr << (inode->i_blkbits - 9); inode->i_blocks = inode->i_blocks > d ? inode->i_blocks - d : 0; } }

/* bmap (VFS-helper): az inode a_ops->bmap-ján át fizikai blokkot ad — a jbd2 a journal-inode
 * első blokkjának (journal-superblock) megtalálásához hívja. A no-op-0 miatt nem találta. */
int bmap(struct inode *inode, sector_t *block)
{
	struct address_space *m = inode ? inode->i_mapping : 0;
	if (!m || !m->a_ops || !m->a_ops->bmap) return -22;
	*block = m->a_ops->bmap(m, *block);
	return 0;
}

/* ===== jbd2 journal-buffer helperek ===== */
/* alloc_buffer_head: a jbd2 a metaadat journalba-másolásához kér egy üres buffer_head-et
 * (jbd2_journal_write_metadata_buffer). A b_data-t majd a folio_set_bh állítja be. */
struct buffer_head *alloc_buffer_head(gfp_t gfp)
{ (void)gfp; return calloc(1, sizeof(struct buffer_head)); }
/* a journal new_bh a forrás-buffer folio-ját KÖLCSÖNZI (folio_set_bh) — itt NEM szabadítjuk,
 * különben a forrás brelse-je dupla-free-t okoz. Csak a buffer_head-et. */
void free_buffer_head(struct buffer_head *bh) { if (bh) free(bh); }
/* folio_set_bh: a buffert egy folio adott offszetére mappeli — b_data = folio_address+offset.
 * Itt a folio _priv-je a lapbázis, így a journal-buffer b_data-ja a forrás-metaadatra mutat. */
void folio_set_bh(struct buffer_head *bh, struct folio *folio, unsigned long offset)
{ bh->b_folio = folio; bh->b_data = (char *)folio_address(folio) + offset; }
void set_bh_page(struct buffer_head *bh, struct page *page, unsigned long offset)
{ bh->b_folio = (struct folio *)page; bh->b_data = (char *)folio_address((struct folio *)page) + offset; }
/* memcpy_from_folio: a folio offszetjéről másol (kmap_local_folio == _priv+offset). */
void memcpy_from_folio(char *to, struct folio *folio, size_t offset, size_t len)
{ if (to && folio) memcpy(to, (char *)folio_address(folio) + offset, len); }

/* filemap_invalidate_lock/unlock: VALÓDI zárolás a mapping->invalidate_lock rw_semaphore-on —
 * az ext4_break_layouts (truncate) WARN_ON(!rwsem_is_locked(...))-ot tesz; a no-op miatt -EINVAL. */
void filemap_invalidate_lock(struct address_space *m) { if (m) down_write(&m->invalidate_lock); }
void filemap_invalidate_unlock(struct address_space *m) { if (m) up_write(&m->invalidate_lock); }
void filemap_invalidate_lock_shared(struct address_space *m) { if (m) down_read(&m->invalidate_lock); }
void filemap_invalidate_unlock_shared(struct address_space *m) { if (m) up_read(&m->invalidate_lock); }
