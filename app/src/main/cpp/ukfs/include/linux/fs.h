/* uKernel hamis <linux/fs.h> — KÉT felület egy fájlban:
 *  1) a régi char-device út (register_chrdev/filp_open/ukernel_register_cdev) — a wifi/firmware használja, NEM törhető;
 *  2) a teljes VFS-objektummodell (super_block/inode/dentry/file/address_space + *_operations) — a valódi
 *     fájlrendszer-driverek (fs/fat, fs/ntfs3, fs/ext4) ehhez fordulnak; a futásidőt az F3-shim adja. */
#ifndef _UK_LINUX_FS_H
#define _UK_LINUX_FS_H

#include <linux/types.h>
#include <linux/compiler.h>   /* __user */
#include <linux/ioctl.h>
#include <linux/fcntl.h>
#include <linux/stat.h>
#include <linux/uidgid.h>
#include <linux/time64.h>
#include <linux/list.h>      /* list_for_each_entry, hlist_for_each_entry */
#include <linux/spinlock.h>
#include <linux/mutex.h>      /* mutex + rw_semaphore */
#include <linux/kernel.h>     /* KERN_*, printk, va_list */
#include <linux/errno.h>
#include <linux/export.h>     /* EXPORT_SYMBOL* */
#include <linux/time.h>       /* sys_tz */
#include <linux/slab.h>       /* GFP_*, kmalloc */
#include <linux/jiffies.h>    /* jiffies */
#include <linux/log2.h>       /* is_power_of_2, rounddown */
#include <linux/uaccess.h>    /* put_user/get_user, copy_*_user */
#include <linux/sched.h>      /* TASK_*, set_current_state */
#include <linux/writeback.h>  /* struct writeback_control */
#include <linux/xarray.h>     /* struct xarray, xa_mark_t (ext4 mballoc/inode) */
#include <linux/percpu-defs.h>/* DEFINE_PER_CPU, this_cpu_inc (ext4 mballoc) */
#include <linux/percpu.h>     /* alloc_percpu/free_percpu (ext4 mballoc) */
#include <linux/cpumask.h>    /* for_each_possible_cpu (ext4 mballoc) */
#include <linux/sizes.h>      /* SZ_1M, SZ_16K (ext4 mballoc) */
#include <linux/overflow.h>   /* DEFINE_RAW_FLEX (ext4 mballoc) */
#include <linux/ratelimit_types.h>
#include <linux/workqueue.h>
#include <linux/kobject.h>
#include <linux/rcupdate.h>
#include <linux/mempool.h>
#include <linux/shrinker.h>
#include <linux/cleanup.h>
#include <linux/fileattr.h>
#include <linux/ioprio.h>
#include <linux/projid.h>
#include <linux/uk_ext4_extra.h>  /* struct ratelimit_state (ntfs3 sb_info) */

struct file;
struct inode;
struct dentry;
struct super_block;
struct address_space;
struct vfsmount;
struct poll_table_struct;
struct module;
struct kstatfs;
struct seq_file;
struct fs_context;
struct fs_parameter_spec;
struct iov_iter;
struct kiocb;
struct page;
struct folio;
struct writeback_control;
struct user_namespace;
struct mnt_idmap;
struct nameidata;
struct kstat;
struct iattr;
struct file_lock;
struct pipe_inode_info;
struct dir_context;
struct fiemap_extent_info;
struct delayed_call { void (*fn)(void *); void *arg; };  /* get_link cleanup-kontextus (l. ukfs_readlink) */
struct posix_acl;
struct backing_dev_info;
struct readahead_control;
struct export_operations;
struct dentry_operations;
struct inode_operations;
struct file_operations;
struct super_operations;
struct file_system_type;

typedef int (*filldir_t)(struct dir_context *, const char *, int, loff_t, u64, unsigned);
struct dir_context { filldir_t actor; loff_t pos; };

#define DT_UNKNOWN	0
#define DT_FIFO		1
#define DT_CHR		2
#define DT_DIR		4
#define DT_BLK		6
#define DT_REG		8
#define DT_LNK		10
#define DT_SOCK		12
#define DT_WHT		14
static inline bool dir_emit(struct dir_context *ctx, const char *name, int namelen, u64 ino, unsigned type)
{ return ctx->actor(ctx, name, namelen, ctx->pos, ino, type) == 0; }
static inline bool dir_emit_dots(struct file *file, struct dir_context *ctx) { (void)file; (void)ctx; return true; }

/* WRAP_DIR_ITER — a zárolt iterate_shared köré (a kernel makró egyszerűsítve) */
#define WRAP_DIR_ITER(x) \
	static int shared_##x(struct file *file, struct dir_context *ctx) \
	{ return x(file, ctx); }

struct name_snapshot { struct qstr_s { union { struct { u32 hash; u32 len; }; u64 hash_len; }; const unsigned char *name; } name; unsigned char inline_name[40]; };
enum rw_hint { WRITE_LIFE_NOT_SET=0, WRITE_LIFE_NONE=1, WRITE_LIFE_SHORT=2, WRITE_LIFE_MEDIUM=3, WRITE_LIFE_LONG=4, WRITE_LIFE_EXTREME=5 };
typedef unsigned int fop_flags_t;
struct io_comp_batch;
struct io_context;
struct swap_info_struct;
struct qstr { union { struct { u32 hash; u32 len; }; u64 hash_len; }; const unsigned char *name; };
#define QSTR_INIT(n, l) { { { .hash = 0, .len = (l) } }, .name = (n) }

#ifndef _UK_VM_FAULT
#define _UK_VM_FAULT
struct vm_area_struct { struct file *vm_file; unsigned long vm_flags; unsigned long vm_start, vm_end, vm_pgoff; struct address_space *vm_mapping; const struct vm_operations_struct *vm_ops; };
struct vm_fault { struct vm_area_struct *vma; unsigned long address; unsigned long pgoff; struct folio *folio; void *page; unsigned int flags; };
struct vm_operations_struct {
	void (*open)(struct vm_area_struct *area);
	void (*close)(struct vm_area_struct *area);
	int (*fault)(struct vm_fault *vmf);
	int (*page_mkwrite)(struct vm_fault *vmf);
	int (*pfn_mkwrite)(struct vm_fault *vmf);
	void (*map_pages)(struct vm_fault *vmf, unsigned long start, unsigned long end);
};
struct folio;
#ifndef PAGECACHE_TAG_DIRTY
#define PAGECACHE_TAG_DIRTY 0
#define PAGECACHE_TAG_WRITEBACK 1
#endif
/* FGP_WRITEBEGIN = FGP_LOCK(0x02)|FGP_WRITE(0x08)|FGP_CREAT(0x04)|FGP_STABLE(0x80) — a VALÓDI
 * kernel-érték; a régi 0x10 nem tartalmazta a LOCK-bitet → a write_begin folio nem zárolódott
 * → az inline-írás ext4_read_inline_folio BUG_ON(!folio_test_locked) elszállt. */
#ifndef FGP_WRITEBEGIN
#define FGP_WRITEBEGIN 0x8E
#endif
struct folio *__filemap_get_folio(struct address_space *mapping, pgoff_t index, int fgp_flags, gfp_t gfp);
struct folio *read_mapping_folio(struct address_space *mapping, pgoff_t index, struct file *file);
void *folio_address(const void *folio);
void folio_put(struct folio *folio);
void folio_lock(struct folio *folio);
void folio_unlock(struct folio *folio);
void folio_mark_uptodate(struct folio *folio);
void folio_mark_dirty(struct folio *folio);
size_t folio_size(const void *folio);
void *kmap_atomic(struct page *page);
void kunmap_atomic(void *addr);
struct folio *folio_alloc(gfp_t gfp, unsigned int order);
void *kmap_local_folio(struct folio *folio, size_t offset);
void kunmap_local(const void *addr);
void *kmap_local_page(struct page *page);
struct page *folio_file_page(struct folio *folio, pgoff_t index);
struct folio *page_folio(struct page *page);
int filemap_page_mkwrite(struct vm_fault *vmf);
struct user_namespace *seq_user_ns(struct seq_file *seq);
struct folio *writeback_iter(struct address_space *mapping, struct writeback_control *wbc, struct folio *folio, int *error);
struct page *read_mapping_page(struct address_space *mapping, pgoff_t index, struct file *file);
char *dentry_path_raw(const struct dentry *dentry, char *buf, int buflen);
int filemap_migrate_folio(struct address_space *m, struct folio *dst, struct folio *src, int mode);
bool iomap_dirty_folio(struct address_space *mapping, struct folio *folio);
void kfree_link(void *p);
void set_delayed_call(struct delayed_call *call, void (*fn)(void *), void *arg);
const char *page_get_link(struct dentry *, struct inode *, struct delayed_call *);
int page_symlink(struct inode *inode, const char *symname, int len);
#define VM_FAULT_LOCKED		0x000400
#define VM_FAULT_RETRY		0x000400
#define VM_FAULT_NOPAGE		0x000100
#define VM_FAULT_SIGBUS		0x000002
#define VM_FAULT_MAJOR		0x000004
#define VM_FAULT_OOM		0x000001
#define VM_FAULT_ERROR		0x000fc0
#define VMA_WRITE_BIT 6
#define VM_WRITE 0x00000002
#define VM_READ 0x00000001
#define VM_EXEC 0x00000004
#define VM_MAYWRITE 0x00000020
#define VM_SHARED		0x00000008
int filemap_fault(struct vm_fault *vmf);
void filemap_map_pages(struct vm_fault *vmf, unsigned long start, unsigned long end);
struct vm_area_desc {
	void *mm; struct file *file;
	unsigned long start, end, pgoff;
	struct file *vm_file;
	unsigned long vma_flags;
	const struct vm_operations_struct *vm_ops;
	void *private_data;
};
int generic_file_mmap_prepare(struct vm_area_desc *desc);
#endif

#ifndef _UK_PAGE_FOLIO
#define _UK_PAGE_FOLIO
struct page { unsigned long flags; struct address_space *mapping; pgoff_t index; void *_priv; };
/* _priv = adatpuffer (folio_address); uk_bh = buffer_head-lánc (folio_buffers, ext4 write) */
struct folio { struct page page; unsigned long flags; struct address_space *mapping; pgoff_t index; void *_priv; void *uk_bh; };
#endif
struct file_ra_state { unsigned long start; unsigned int size; unsigned int async_size; unsigned int ra_pages; unsigned int mmap_miss; loff_t prev_pos; };
struct block_device {
	dev_t			bd_dev;
	struct super_block	*bd_super;
	void			*bd_priv;     /* shim: a libukblk-eszközhöz */
	unsigned		bd_block_size;
	struct address_space	*bd_mapping;
};

#if !defined(__BIONIC__)   /* bionic's <asm-generic/posix_types.h> already provides it */
#ifndef _UK_KERNEL_FSID
#define _UK_KERNEL_FSID
typedef struct { int val[2]; } __kernel_fsid_t;
#endif
#endif

struct mapping_metadata_bhs {
	struct address_space *mapping;
	spinlock_t lock;
	struct list_head list;
};
enum fs_update_time { FS_UPD_ATIME, FS_UPD_CMTIME };

/* superblock-flagek — KORÁN, mert a lenti inline-ok (sb_rdonly) használják */
#define SB_RDONLY	1
#define SB_NOSUID	2
#define SB_NODEV	4
#define SB_NOEXEC	8
#define SB_SYNCHRONOUS	16
#define SB_MANDLOCK	64
#define SB_DIRSYNC	128
#define SB_NOATIME	1024
#define SB_NODIRATIME	2048
#define SB_SILENT	32768
#define SB_POSIXACL	(1 << 16)
#define SB_ACTIVE	(1 << 30)
#define FSLABEL_MAX 256
#define MAX_LFS_FILESIZE 0x7fffffffffffffffLL
#define SEEK_DATA 3
#define SEEK_HOLE 4
#define SB_FREEZE_WRITE 1
#define SB_FREEZE_PAGEFAULT 2
#define SB_FREEZE_FS 3
#define SB_FREEZE_COMPLETE 4
#define SB_UNFROZEN 0
#define MS_RDONLY	SB_RDONLY

#define IS_RDONLY(inode)	sb_rdonly((inode)->i_sb)
#define IS_SYNC(inode)		0
#define IS_DIRSYNC(inode)	0
#define IS_DEADDIR(inode)	((inode)->i_flags & 32)
#define IS_IMMUTABLE(inode)	((inode)->i_flags & S_IMMUTABLE)
#define IS_APPEND(inode)	0
#define IS_NOATIME(inode)	0
#define S_DEAD			32
#define S_APPEND		4
#define S_SYNC			1
#define S_DIRSYNC		64
#define S_ENCRYPTED		(1 << 14)
#define S_CASEFOLD		(1 << 15)
#define S_NOSEC (1 << 13)
#define S_VERITY		(1 << 16)

#define RENAME_NOREPLACE	(1 << 0)
#define RENAME_EXCHANGE		(1 << 1)
#define RENAME_WHITEOUT		(1 << 2)

/* iattr-attribútum flagek (setattr) — a FAT ATTR_RO/DIR/... a msdos_fs.h-ban van, ütközés nincs */
#define ATTR_MODE	(1 << 0)
#define ATTR_UID	(1 << 1)
#define ATTR_GID	(1 << 2)
#define ATTR_SIZE	(1 << 3)
#define ATTR_ATIME	(1 << 4)
#define ATTR_MTIME	(1 << 5)
#define ATTR_CTIME	(1 << 6)
#define ATTR_ATIME_SET	(1 << 7)
#define ATTR_MTIME_SET	(1 << 8)
#define ATTR_FORCE	(1 << 9)
#define ATTR_KILL_SUID	(1 << 11)
#define ATTR_KILL_SGID	(1 << 12)
#define ATTR_FILE	(1 << 13)
#define ATTR_TIMES_SET	(1 << 16)

#define MAY_EXEC	0x00000001
#define MAY_WRITE	0x00000002
#define MAY_READ	0x00000004
#define MAY_APPEND	0x00000008
#define MAY_ACCESS	0x00000010
#define MAY_OPEN	0x00000020

static inline kuid_t current_fsuid(void) { return GLOBAL_ROOT_UID; }
static inline kgid_t current_fsgid(void) { return GLOBAL_ROOT_GID; }
static inline kuid_t current_uid(void) { return GLOBAL_ROOT_UID; }
static inline kgid_t current_gid(void) { return GLOBAL_ROOT_GID; }
static inline kuid_t from_vfsuid(struct mnt_idmap *idmap, struct user_namespace *ns, vfsuid_t vfsuid) { (void)idmap; (void)ns; return (kuid_t){ .val = vfsuid.val }; }
static inline kgid_t from_vfsgid(struct mnt_idmap *idmap, struct user_namespace *ns, vfsgid_t vfsgid) { (void)idmap; (void)ns; return (kgid_t){ .val = vfsgid.val }; }
static inline bool vfsuid_eq_kuid(vfsuid_t v, kuid_t k) { return v.val == k.val; }
static inline bool vfsgid_eq_kgid(vfsgid_t v, kgid_t k) { return v.val == k.val; }

struct kstat {
	u32		result_mask;
	umode_t		mode;
	unsigned int	nlink;
	u32		blksize;
	u64		attributes;
	u64		attributes_mask;
	u64		ino;
	dev_t		dev;
	dev_t		rdev;
	kuid_t		uid;
	kgid_t		gid;
	loff_t		size;
	struct timespec64 atime;
	struct timespec64 mtime;
	struct timespec64 ctime;
	struct timespec64 btime;
	u64		blocks;
	u32		dio_mem_align;
	u32		dio_offset_align;
};
#define STATX_INO	0x00000100U
#define STATX_BTIME	0x00000800U
#define STATX_ATTR_COMPRESSED	0x4
#define STATX_ATTR_IMMUTABLE	0x10
#define STATX_ATTR_APPEND	0x20
#define STATX_ATTR_ENCRYPTED	0x800

struct iattr {
	unsigned int	ia_valid;
	umode_t		ia_mode;
	union { kuid_t ia_uid; vfsuid_t ia_vfsuid; };
	union { kgid_t ia_gid; vfsgid_t ia_vfsgid; };
	loff_t		ia_size;
	struct timespec64 ia_atime;
	struct timespec64 ia_mtime;
	struct timespec64 ia_ctime;
	struct file	*ia_file;
};

/* ===== address_space (page cache felület) ===== */
struct address_space_operations {
	int (*read_folio)(struct file *, struct folio *);
	int (*writepages)(struct address_space *, struct writeback_control *);
	bool (*dirty_folio)(struct address_space *, struct folio *);
	void (*readahead)(struct readahead_control *);
	int (*write_begin)(const struct kiocb *, struct address_space *, loff_t pos, unsigned len, struct folio **foliop, void **fsdata);
	int (*write_end)(const struct kiocb *, struct address_space *, loff_t pos, unsigned len, unsigned copied, struct folio *folio, void *fsdata);
	sector_t (*bmap)(struct address_space *, sector_t);
	void (*invalidate_folio)(struct folio *, size_t offset, size_t len);
	int (*release_folio)(struct folio *, gfp_t);
	ssize_t (*direct_IO)(struct kiocb *, struct iov_iter *);
	int (*migrate_folio)(struct address_space *, struct folio *, struct folio *, int);
	bool (*is_partially_uptodate)(struct folio *, size_t from, size_t count);
	int (*swap_activate)(struct swap_info_struct *sis, struct file *file, sector_t *span);
	int (*error_remove_folio)(struct address_space *, struct folio *);
};

struct address_space {
	struct inode *host;
	const struct address_space_operations *a_ops;
	unsigned long nrpages;
	pgoff_t writeback_index;
	struct rw_semaphore invalidate_lock;
	unsigned long flags;
	gfp_t gfp_mask;
	void *private_data;
	atomic_t i_mmap_writable;
	errseq_t wb_err;
};

/* ===== inode ===== */
struct inode {
	umode_t			i_mode;
	unsigned short		i_opflags;
	kuid_t			i_uid;
	kgid_t			i_gid;
	unsigned int		i_flags;
	const struct inode_operations *i_op;
	struct super_block	*i_sb;
	struct address_space	*i_mapping;
	unsigned long		i_ino;
	union { const unsigned int i_nlink; unsigned int __i_nlink; };
	dev_t			i_rdev;
	loff_t			i_size;
	struct timespec64	i_atime;
	struct timespec64	i_mtime;
	struct timespec64	__i_ctime;
	unsigned int		i_blkbits;
	u64			i_version;
	blkcnt_t		i_blocks;
	unsigned long		i_state;
	unsigned int		i_generation;
	const struct file_operations *i_fop;
	struct address_space	i_data;
	void			*i_private;
	struct list_head	i_sb_list;
	struct hlist_node	i_hash;
	atomic_t		i_count;
	struct rw_semaphore	i_rwsem;
	struct list_head	i_io_list;
	spinlock_t		i_lock;
	unsigned short		i_bytes;
	enum rw_hint		i_write_hint;
	struct posix_acl	*i_acl;
	struct posix_acl	*i_default_acl;
	atomic_t		i_writecount;
	atomic_t		i_readcount;
	char			*i_link;        /* fast-symlink gyorsított célja (inode_set_cached_link) */
	int			i_linklen;
};

#define I_NEW		(1 << 3)
#define I_FREEING	(1 << 5)
#define I_CLEAR		(1 << 6)
#define I_SYNC		(1 << 7)
#define I_REFERENCED	(1 << 8)
#define I_DIRTY_TIME	(1 << 11)
#define I_LINKABLE	(1 << 10)
#define I_WB_SWITCH	(1 << 13)
#define I_OVL_INUSE	(1 << 14)
#define I_CREATING	(1 << 15)
#define I_DONTCACHE	(1 << 16)
#define I_DIRTY_SYNC	(1 << 0)
#define I_DIRTY_DATASYNC (1 << 1)
#define I_DIRTY_PAGES	(1 << 2)
#define S_IMMUTABLE	8
#define S_NOATIME	256
#define S_NOCMTIME	1024

/* ===== super_block ===== */
struct super_block {
	unsigned long		s_blocksize;
	unsigned char		s_blocksize_bits;
	loff_t			s_maxbytes;
	struct file_system_type	*s_type;
	const struct super_operations *s_op;
	const struct export_operations *s_export_op;
	unsigned long		s_flags;
	unsigned long		s_magic;
	struct dentry		*s_root;
	void			*s_fs_info;
	u32			s_time_gran;
	time64_t		s_time_min;
	time64_t		s_time_max;
	char			s_id[32];
	struct block_device	*s_bdev;      /* shim: block_device — a libukblk eszköz */
	struct backing_dev_info	*s_bdi;
	dev_t			s_dev;
	struct list_head	s_inodes;
	void			*s_d_op;
	unsigned int		s_max_links;
	struct rw_semaphore	s_umount;
	unsigned long		s_iflags;
	struct { int frozen; } s_writers;
	const struct xattr_handler * const *s_xattr;
	void			*s_shrink;
	u8			s_uuid[16];
};

struct super_operations {
	struct inode *(*alloc_inode)(struct super_block *sb);
	void (*destroy_inode)(struct inode *);
	void (*free_inode)(struct inode *);
	void (*dirty_inode)(struct inode *, int flags);
	int (*write_inode)(struct inode *, struct writeback_control *wbc);
	int (*drop_inode)(struct inode *);
	void (*evict_inode)(struct inode *);
	void (*put_super)(struct super_block *);
	int (*sync_fs)(struct super_block *sb, int wait);
	int (*statfs)(struct dentry *, struct kstatfs *);
	int (*remount_fs)(struct super_block *, int *, char *);
	int (*freeze_fs)(struct super_block *sb);
	int (*unfreeze_fs)(struct super_block *sb);
	int (*freeze_super)(struct super_block *, void *);
	int (*thaw_super)(struct super_block *, void *);
	int (*show_options)(struct seq_file *, struct dentry *);
	long (*free_cached_objects)(struct super_block *, void *);
	void (*shutdown)(struct super_block *sb);
	void (*evict_inode_final)(struct inode *);
};

struct readahead_control {
	struct file		*file;
	struct address_space	*mapping;
	struct inode		*inode;
	unsigned long		_index;
	unsigned int		_nr_pages;
	unsigned int		_batch_count;
};

/* ===== dentry ===== */
struct dentry {
	struct super_block	*d_sb;
	struct inode		*d_inode;
	struct dentry		*d_parent;
	struct qstr		d_name;
	const struct dentry_operations *d_op;
	void			*d_fsdata;
	unsigned int		d_flags;
	struct list_head	d_child;
	unsigned char		d_iname[40];
};
#define DCACHE_DISCONNECTED	0x00000004
#define DCACHE_NFSFS_RENAMED	0x00000008
#define DCACHE_OP_HASH		0x00000001
#define DCACHE_OP_COMPARE	0x00000002

struct dentry_operations {
	int (*d_revalidate)(struct inode *, const struct qstr *, struct dentry *, unsigned int);
	int (*d_weak_revalidate)(struct dentry *, unsigned int);
	int (*d_hash)(const struct dentry *, struct qstr *);
	int (*d_compare)(const struct dentry *, unsigned int, const char *, const struct qstr *);
	int (*d_delete)(const struct dentry *);
	void (*d_release)(struct dentry *);
	void (*d_prune)(struct dentry *);
};

struct path { struct vfsmount *mnt; struct dentry *dentry; };

/* ===== file + file_operations (TELJES VFS, a régi mezőkkel KOMPATIBILIS) ===== */
struct file {
	void			*private_data;
	unsigned int		f_flags;
	fmode_t			f_mode;
	loff_t			f_pos;
	struct path		f_path;
	const struct file_operations *f_op;
	struct inode		*f_inode;
	struct address_space	*f_mapping;
	u64			f_version;
	struct file_ra_state	f_ra;
};

struct file_operations {
	struct module *owner;
	loff_t  (*llseek)(struct file *, loff_t, int);
	ssize_t (*read)(struct file *, char __user *, size_t, loff_t *);
	ssize_t (*write)(struct file *, const char __user *, size_t, loff_t *);
	ssize_t (*read_iter)(struct kiocb *, struct iov_iter *);
	ssize_t (*write_iter)(struct kiocb *, struct iov_iter *);
	int     (*iterate_shared)(struct file *, struct dir_context *);
	unsigned int (*poll)(struct file *, struct poll_table_struct *);
	long    (*unlocked_ioctl)(struct file *, unsigned int, unsigned long);
	long    (*compat_ioctl)(struct file *, unsigned int, unsigned long);
	int     (*mmap)(struct file *, void *);
	int     (*mmap_prepare)(void *desc);
	int     (*open)(struct inode *, struct file *);
	int     (*flush)(struct file *, void *id);
	int     (*release)(struct inode *, struct file *);
	int     (*fsync)(struct file *, loff_t, loff_t, int datasync);
	long    (*fallocate)(struct file *, int, loff_t, loff_t);
	ssize_t (*splice_read)(struct file *, loff_t *, struct pipe_inode_info *, size_t, unsigned int);
	ssize_t (*splice_write)(struct pipe_inode_info *, struct file *, loff_t *, size_t, unsigned int);
	unsigned long (*get_unmapped_area)(struct file *, unsigned long, unsigned long, unsigned long, unsigned long);
	int     (*setlease)(struct file *, int, void **, void **);
	fop_flags_t fop_flags;
	int     (*iopoll)(struct kiocb *kiocb, struct io_comp_batch *, unsigned int flags);
};
int generic_setlease(struct file *, int, void **, void **);
long compat_ptr_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
ssize_t iter_file_splice_write(struct pipe_inode_info *, struct file *, loff_t *, size_t, unsigned int);

struct inode_operations {
	struct dentry *(*lookup)(struct inode *, struct dentry *, unsigned int);
	int (*create)(struct mnt_idmap *, struct inode *, struct dentry *, umode_t, bool);
	int (*link)(struct dentry *, struct inode *, struct dentry *);
	int (*unlink)(struct inode *, struct dentry *);
	int (*symlink)(struct mnt_idmap *, struct inode *, struct dentry *, const char *);
	struct dentry *(*mkdir)(struct mnt_idmap *, struct inode *, struct dentry *, umode_t);
	int (*rmdir)(struct inode *, struct dentry *);
	int (*mknod)(struct mnt_idmap *, struct inode *, struct dentry *, umode_t, dev_t);
	int (*rename)(struct mnt_idmap *, struct inode *, struct dentry *, struct inode *, struct dentry *, unsigned int);
	int (*setattr)(struct mnt_idmap *, struct dentry *, struct iattr *);
	int (*getattr)(struct mnt_idmap *, const struct path *, struct kstat *, u32, unsigned int);
	int (*fiemap)(struct inode *, struct fiemap_extent_info *, u64 start, u64 len);
	int (*update_time)(struct inode *, enum fs_update_time, umode_t);
	int (*tmpfile)(struct mnt_idmap *, struct inode *, struct file *, umode_t);
	void *(*get_acl)(struct mnt_idmap *, struct dentry *, int);
	void *(*get_inode_acl)(struct inode *, int, bool);
	int (*set_acl)(struct mnt_idmap *, struct dentry *, void *, int);
	int (*fileattr_get)(struct dentry *dentry, struct file_kattr *fa);
	int (*fileattr_set)(struct mnt_idmap *, struct dentry *, struct file_kattr *);
	int (*listxattr)(struct dentry *, char *, size_t);
	const char *(*get_link)(struct dentry *, struct inode *, struct delayed_call *);
};

/* ===== file_system_type ===== */
struct file_system_type {
	const char *name;
	int fs_flags;
	int (*init_fs_context)(struct fs_context *);
	const struct fs_parameter_spec *parameters;
	struct dentry *(*mount)(struct file_system_type *, int, const char *, void *);
	void (*kill_sb)(struct super_block *);
	struct module *owner;
	struct file_system_type *next;
	void *fs_supers;
};
#define FS_REQUIRES_DEV		1
#define FS_USERNS_MOUNT		8
#define FS_ALLOW_IDMAP		32

int register_filesystem(struct file_system_type *);
int unregister_filesystem(struct file_system_type *);
void kill_block_super(struct super_block *sb);
struct dentry *mount_bdev(struct file_system_type *fs_type, int flags,
	const char *dev_name, void *data,
	int (*fill_super)(struct super_block *, void *, int));

/* ===== a futásidejű VFS-segédek (az F3-shim implementálja) ===== */
void *alloc_inode_sb(struct super_block *sb, struct kmem_cache *cache, gfp_t gfp);
struct inode *new_inode(struct super_block *sb);
struct inode *iget_locked(struct super_block *sb, unsigned long ino);
struct inode *iget5_locked(struct super_block *sb, unsigned long hashval,
	int (*test)(struct inode *, void *), int (*set)(struct inode *, void *), void *data);
void unlock_new_inode(struct inode *inode);
void iget_failed(struct inode *inode);
void iput(struct inode *inode);
struct inode *igrab(struct inode *inode);
int insert_inode_locked(struct inode *inode);
void insert_inode_hash(struct inode *inode);
void remove_inode_hash(struct inode *inode);
void clear_inode(struct inode *inode);
void make_bad_inode(struct inode *inode);
int is_bad_inode(struct inode *inode);
void inode_init_once(struct inode *inode);
void ihold(struct inode *inode);

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef NAME_MAX
#define NAME_MAX 255
#endif

struct dentry *d_obtain_alias(struct inode *inode);
struct dentry *d_make_root(struct inode *root_inode);
struct dentry *d_alloc_anon(struct super_block *sb);
void d_instantiate(struct dentry *dentry, struct inode *inode);
struct dentry *d_splice_alias(struct inode *inode, struct dentry *dentry);
struct dentry *d_find_alias(struct inode *inode);
void d_add(struct dentry *dentry, struct inode *inode);
void dput(struct dentry *dentry);
struct dentry *dget(struct dentry *dentry);

extern struct user_namespace init_user_ns;
static inline struct user_namespace *i_user_ns(const struct inode *inode) { (void)inode; return &init_user_ns; }
static inline vfsuid_t i_uid_into_vfsuid(struct mnt_idmap *idmap, const struct inode *inode) { (void)idmap; return (vfsuid_t){ .val = inode->i_uid.val }; }
static inline vfsgid_t i_gid_into_vfsgid(struct mnt_idmap *idmap, const struct inode *inode) { (void)idmap; return (vfsgid_t){ .val = inode->i_gid.val }; }
/* chown-helperek — a VALÓDI kernel-inline-ok (eddig no-op stubok voltak az ext4_stubs.c-ben,
 * ezért a chown nem frissítette az i_uid/i_gid-et). Az ATTR_UID/GID-nél alkalmazzák az új értéket. */
static inline bool i_uid_needs_update(struct mnt_idmap *idmap, const struct iattr *attr, const struct inode *inode)
{ return (attr->ia_valid & ATTR_UID) && !vfsuid_eq_kuid(attr->ia_vfsuid, inode->i_uid); }
static inline bool i_gid_needs_update(struct mnt_idmap *idmap, const struct iattr *attr, const struct inode *inode)
{ return (attr->ia_valid & ATTR_GID) && !vfsgid_eq_kgid(attr->ia_vfsgid, inode->i_gid); }
static inline void i_uid_update(struct mnt_idmap *idmap, const struct iattr *attr, struct inode *inode)
{ if (attr->ia_valid & ATTR_UID) inode->i_uid = from_vfsuid(idmap, i_user_ns(inode), attr->ia_vfsuid); }
static inline void i_gid_update(struct mnt_idmap *idmap, const struct iattr *attr, struct inode *inode)
{ if (attr->ia_valid & ATTR_GID) inode->i_gid = from_vfsgid(idmap, i_user_ns(inode), attr->ia_vfsgid); }
/* i_uid_read/write: az ext4_fill_raw_inode ezzel olvassa az inode uid-jét a raw-inode-ba íráshoz,
 * az ext4_iget ezzel írja vissza — eddig no-op stubok voltak, ezért a chown nem perzisztált. */
static inline uid_t i_uid_read(const struct inode *inode) { return inode->i_uid.val; }
static inline gid_t i_gid_read(const struct inode *inode) { return inode->i_gid.val; }
static inline void i_uid_write(struct inode *inode, uid_t uid) { inode->i_uid.val = uid; }
static inline void i_gid_write(struct inode *inode, gid_t gid) { inode->i_gid.val = gid; }
static inline struct inode *d_inode(const struct dentry *dentry) { return dentry->d_inode; }
static inline struct inode *d_backing_inode(const struct dentry *dentry) { return dentry->d_inode; }

struct timespec64 simple_inode_init_ts(struct inode *inode);
static inline struct inode *d_inode_rcu(const struct dentry *dentry) { return dentry->d_inode; }
static inline bool d_really_is_negative(const struct dentry *dentry) { return dentry->d_inode == 0; }
static inline bool d_really_is_positive(const struct dentry *dentry) { return dentry->d_inode != 0; }
static inline bool d_is_dir(const struct dentry *dentry) { return dentry->d_inode && S_ISDIR(dentry->d_inode->i_mode); }
static inline const unsigned char *d_name_name(const struct dentry *d) { return d->d_name.name; }
static inline loff_t i_size_read(const struct inode *inode) { return inode->i_size; }
static inline void i_size_write(struct inode *inode, loff_t i_size) { inode->i_size = i_size; }
static inline void set_nlink(struct inode *inode, unsigned int nlink) { inode->__i_nlink = nlink; }
static inline void inc_nlink(struct inode *inode) { inode->__i_nlink++; }
static inline void drop_nlink(struct inode *inode) { inode->__i_nlink--; }
static inline void clear_nlink(struct inode *inode) { inode->__i_nlink = 0; }
static inline void inode_inc_iversion(struct inode *inode) { inode->i_version++; }
#ifndef _UK_FILE_INODE
#define _UK_FILE_INODE
static inline struct inode *file_inode(const struct file *f) { return f->f_inode; }
#endif
static inline struct dentry *file_dentry(const struct file *f) { return f->f_path.dentry; }
static inline int sb_rdonly(const struct super_block *sb) { return sb->s_flags & SB_RDONLY; }
static inline bool inode_is_locked(struct inode *i) { (void)i; return true; }

static inline int sb_set_blocksize(struct super_block *sb, int size)
{ sb->s_blocksize = size; sb->s_blocksize_bits = 0; while ((1 << sb->s_blocksize_bits) < size) sb->s_blocksize_bits++; return size; }
static inline int sb_min_blocksize(struct super_block *sb, int size) { return sb_set_blocksize(sb, size); }

static inline struct timespec64 inode_get_mtime(const struct inode *inode) { return inode->i_mtime; }
static inline struct timespec64 inode_get_atime(const struct inode *inode) { return inode->i_atime; }
static inline struct timespec64 inode_get_ctime(const struct inode *inode) { return inode->__i_ctime; }
static inline struct timespec64 inode_set_mtime_to_ts(struct inode *inode, struct timespec64 ts) { inode->i_mtime = ts; return ts; }
static inline struct timespec64 inode_set_atime_to_ts(struct inode *inode, struct timespec64 ts) { inode->i_atime = ts; return ts; }
static inline struct timespec64 inode_set_ctime_to_ts(struct inode *inode, struct timespec64 ts) { inode->__i_ctime = ts; return ts; }
static inline struct timespec64 inode_set_mtime(struct inode *inode, time64_t sec, long nsec) { struct timespec64 t = { sec, nsec }; inode->i_mtime = t; return t; }
static inline struct timespec64 inode_set_atime(struct inode *inode, time64_t sec, long nsec) { struct timespec64 t = { sec, nsec }; inode->i_atime = t; return t; }
static inline struct timespec64 inode_set_ctime(struct inode *inode, time64_t sec, long nsec) { struct timespec64 t = { sec, nsec }; inode->__i_ctime = t; return t; }
struct timespec64 inode_set_ctime_current(struct inode *inode);
struct timespec64 current_time(struct inode *inode);
static inline __kernel_fsid_t u64_to_fsid(u64 v) { __kernel_fsid_t f; f.val[0] = (u32)v; f.val[1] = (u32)(v >> 32); return f; }
void truncate_setsize(struct inode *inode, loff_t newsize);
void truncate_inode_pages(struct address_space *mapping, loff_t lstart);
void truncate_inode_pages_final(struct address_space *mapping);

/* ===== további VFS-bitek a fat-hoz ===== */
#define IOCB_DIRECT    (1<<2)
#define IOCB_NOWAIT    (1<<3)
#define IOCB_APPEND    (1<<1)
#define IOCB_SYNC      (1<<0)
#define IOCB_DSYNC     (1<<4)
struct kiocb {
	struct file	*ki_filp;
	loff_t		ki_pos;
	int		ki_flags;
};
struct fstrim_range { u64 start; u64 len; u64 minlen; };
#define FITRIM		_IOWR('X', 121, struct fstrim_range)
#define FS_IOC_GETFSLABEL _IOR(0x94, 49, char[FSLABEL_MAX])
#define FS_IOC_SETFSLABEL _IOW(0x94, 50, char[FSLABEL_MAX])
#define FICLONE		_IOW(0x94, 9, int)

struct inode *ilookup(struct super_block *sb, unsigned long ino);
struct inode *find_inode_by_ino_rcu(struct super_block *sb, unsigned long ino);
char *__getname(void);
void __putname(const char *name);
extern struct mnt_idmap nop_mnt_idmap;
static inline struct mnt_idmap *file_mnt_idmap(struct file *file) { (void)file; return &nop_mnt_idmap; }
static inline bool inode_owner_or_capable(struct mnt_idmap *idmap, const struct inode *inode) { (void)idmap; (void)inode; return true; }
int setattr_prepare(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *attr);
void setattr_copy(struct mnt_idmap *idmap, struct inode *inode, const struct iattr *attr);
void generic_fillattr(struct mnt_idmap *idmap, u32 request_mask, struct inode *inode, struct kstat *stat);
extern struct user_namespace init_user_ns;
extern const struct file_operations generic_ro_fops;
ssize_t generic_read_dir(struct file *, char __user *, size_t, loff_t *);
ssize_t filemap_splice_read(struct file *in, loff_t *ppos, struct pipe_inode_info *pipe, size_t len, unsigned int flags);
ssize_t generic_file_splice_read(struct file *, loff_t *, struct pipe_inode_info *, size_t, unsigned int);
ssize_t generic_file_read_iter(struct kiocb *, struct iov_iter *);
ssize_t generic_file_write_iter(struct kiocb *, struct iov_iter *);
loff_t generic_file_llseek(struct file *file, loff_t offset, int whence);

int generic_file_fsync(struct file *, loff_t, loff_t, int);


/* === ext4 extra (workflow) — buffer/folio/dentry segédek === */
struct buffer_head *bdev_getblk(struct block_device *bdev, sector_t block, unsigned size, gfp_t gfp);
struct buffer_head *getblk_unmovable(struct block_device *bdev, sector_t block, unsigned size);
struct buffer_head *sb_getblk_gfp(struct super_block *sb, sector_t block, gfp_t gfp);
bool block_is_partially_uptodate(struct folio *folio, size_t from, size_t count);
int generic_error_remove_folio(struct address_space *mapping, struct folio *folio);
bool noop_dirty_folio(struct address_space *mapping, struct folio *folio);
void *folio_zero_tail(struct folio *folio, size_t offset, void *kaddr);
unsigned long folio_nr_pages(const struct folio *folio);
static inline void *folio_get_private(const struct folio *folio) { return folio->uk_bh; }
static inline void folio_attach_private(struct folio *folio, void *data) { folio->uk_bh = data; }
static inline void *folio_detach_private(struct folio *folio) { void *p = folio->uk_bh; folio->uk_bh = NULL; return p; }
#define folio_buffers(folio) ((struct buffer_head *)folio_get_private(folio))
struct dentry *d_find_any_alias(struct inode *inode);
struct dentry *dget_parent(struct dentry *dentry);
unsigned long thp_get_unmapped_area(struct file *filp, unsigned long addr, unsigned long len, unsigned long pgoff, unsigned long flags);
char *d_path(const struct path *path, char *buf, int buflen);
static inline __kernel_fsid_t uuid_to_fsid(__u8 *uuid){ u64 a,b; __builtin_memcpy(&a,uuid,8); __builtin_memcpy(&b,uuid+8,8); return u64_to_fsid(a^b); }
extern struct kobject *fs_kobj;
static inline struct folio *readahead_folio(struct readahead_control *rac){(void)rac;return ((void*)0);}
static inline pgoff_t readahead_index(const struct readahead_control *rac){(void)rac;return 0;}
static inline unsigned int i_blocksize(const struct inode *node){ return 1U << node->i_blkbits; }
#define IS_ROOT(de) ((de)->d_parent == (de))
/* makró — a `pos` lehet egész (loff_t) VAGY pointer (pl. bh->b_data, jbd2) */
#define offset_in_folio(folio, pos) ((size_t)((unsigned long)(pos) & (PAGE_SIZE - 1)))
static inline loff_t readahead_pos(struct readahead_control *rac){(void)rac;return 0;}
static inline size_t readahead_length(struct readahead_control *rac){(void)rac;return 0;}
static inline unsigned int readahead_count(const struct readahead_control *rac){(void)rac;return 0;}
char *file_path(struct file *, char *, int);
char *kmemdup_nul(const char *s, size_t len, gfp_t gfp);
const char *simple_get_link(struct dentry *, struct inode *, struct delayed_call *);
struct block_device *file_bdev(struct file *bdev_file);
struct kobject *kobject_create_and_add(const char *name, struct kobject *parent);
void sysfs_notify(struct kobject *kobj, const char *dir, const char *attr);
extern const struct qstr dotdot_name;
struct mb_cache_entry *mb_cache_entry_delete_or_get(struct mb_cache *cache, u32 key, u64 value);
void mb_cache_entry_wait_unused(struct mb_cache_entry *entry);

/* ===== RÉGI char-device út — VÁLTOZATLAN (wifi/firmware) ===== */
struct file *filp_open(const char *name, int flags, int mode);
int  filp_close(struct file *f, void *id);
long kernel_read(struct file *f, void *buf, unsigned long count, loff_t *pos);
long kernel_write(struct file *f, const void *buf, unsigned long count, loff_t *pos);
long vfs_read(struct file *f, char __user *buf, unsigned long count, loff_t *pos);
long vfs_write(struct file *f, const char __user *buf, unsigned long count, loff_t *pos);

#ifndef O_NONBLOCK
#define O_NONBLOCK 0x0800
#endif
#define FMODE_READ  0x1
#define FMODE_WRITE 0x2
#define FMODE_CAN_READ  0x20000
#define FMODE_CAN_WRITE 0x40000
#define FMODE_NOWAIT 0x8000000
#define FMODE_CAN_ODIRECT 0x400000
#define FMODE_32BITHASH 0x200
#define FMODE_64BITHASH 0x400
#define FMODE_EXEC 0x20
#define FMODE_RANDOM 0x1000
#define FMODE_EXEC 0x20

int  register_chrdev(unsigned int major, const char *name, const struct file_operations *fops);
void unregister_chrdev(unsigned int major, const char *name);
int  ukernel_register_cdev(const char *name, const struct file_operations *fops, void *drvdata);
void ukernel_unregister_cdev(const char *name);

#endif
