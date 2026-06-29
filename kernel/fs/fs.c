/* On-disk storage for StinkOS, multi-mount edition.
 *
 * The kernel keeps a fixed-size table of mount slots. Each slot binds a
 * (drive, dir_lba, data_lba, data_end) tuple to a filesystem backend
 * via an `fs_ops` callback table. The bundled backend is StinkFS, a
 * small writable filesystem of named files; the ops indirection is
 * cheap (single static dispatch today) but lets a future backend
 * (FATFS / DevFS / ProcFS) plug in without re-plumbing every caller.
 *
 * Slot 0 (the "A:" mount) is auto-registered at fs_init with the
 * built-in StinkFS at the Makefile-pinned LBAs on ATA drive 0. The
 * public fs_file_* API today resolves to slot 0 for backward compat;
 * the prefix-routed lookup (A:/B:) lands in a follow-up commit.
 *
 * Disk layout for the primary StinkFS (must match the Makefile):
 *   LBA 0       : boot sector
 *   LBA 1-127   : kernel (127 sectors, up to 63.5 KiB)
 *   LBA 128-129 : StinkFS directory (2 sectors)
 *   LBA 130+    : StinkFS data (~100 MiB) */
#include "fs.h"
#include "ata.h"

#define STINKFS_MAGIC   0x4B4E5453u   /* 'S','T','N','K' little-endian */
#define FS_DIR_LBA      512
#define FS_DIR_SECTORS  2
#define FS_DATA_LBA     514
#define FS_DATA_END     200514
#define FS_MAX_FILES    40            /* fits in 2 sectors */
#define FS_MAX_MOUNTS   2             /* A:, B: */

struct fs_file {
	char         name[16];
	unsigned int start;
	unsigned int size;
} __attribute__((packed));

struct fs_dir {
	unsigned int   magic;
	unsigned int   count;
	unsigned int   next_free;
	struct fs_file files[FS_MAX_FILES];
} __attribute__((packed));

struct fs_mount;

/* Backend ops table. Future non-StinkFS backends (FATFS, DevFS) plug
 * in by registering their own `fs_ops` struct + state pointer in a
 * mount slot; the public fs_file_* wrappers dispatch via this table. */
struct fs_ops {
	int (*file_write)     (struct fs_mount *, const char *, const void *, unsigned int);
	int (*file_write_at)  (struct fs_mount *, const char *, const void *, unsigned int, unsigned int);
	int (*file_append)    (struct fs_mount *, const char *, const void *, unsigned int);
	int (*file_read_at)   (struct fs_mount *, const char *, void *, unsigned int, unsigned int);
	int (*file_delete)    (struct fs_mount *, const char *);
	int (*file_count)     (struct fs_mount *);
	int (*file_info)      (struct fs_mount *, int, char *);
	int (*file_size)      (struct fs_mount *, const char *);
	int (*file_lba_sectors)(struct fs_mount *, const char *, unsigned int *, unsigned int *);
};

/* One mount slot. Each slot owns its dir buffer (2 sectors) + bounce
 * IO buffer + StinkFS data-region bounds + which ATA drive the disk
 * lives on. `present` is 0 until sys_mount registers the slot. */
struct fs_mount {
	int                 present;
	int                 drive;          /* 0..3, ata_drive_* index */
	unsigned int        dir_lba;
	unsigned int        data_lba;
	unsigned int        data_end;
	const struct fs_ops *ops;
	unsigned char       dir_buf[512 * FS_DIR_SECTORS];
	unsigned char       io_buf[512];
};

static struct fs_mount mounts[FS_MAX_MOUNTS];

static struct fs_dir *mount_dir(struct fs_mount *m)
{
	return (struct fs_dir *)m->dir_buf;
}

/* ---- StinkFS backend ---- */

static int stinkfs_dir_load(struct fs_mount *m)
{
	if (ata_drive_read(m->drive, m->dir_lba, FS_DIR_SECTORS, m->dir_buf) != 0)
		return -1;
	struct fs_dir *d = mount_dir(m);
	if (d->magic != STINKFS_MAGIC) {
		d->magic     = STINKFS_MAGIC;
		d->count     = 0;
		d->next_free = m->data_lba;
	}
	return 0;
}

static int stinkfs_dir_save(struct fs_mount *m)
{
	return ata_drive_write(m->drive, m->dir_lba, FS_DIR_SECTORS, m->dir_buf);
}

/* ---- name helpers ---- */

/* Names always fit in 16 bytes. Dir-entry names are zero-padded; caller
 * literals are NUL-terminated. Stop at the first NUL in EITHER string. */
static int name_eq(const char *a, const char *b)
{
	for (int i = 0; i < 16; i++) {
		if (a[i] != b[i]) return 0;
		if (a[i] == 0)    return 1;
	}
	return 1;
}

static int name_ci_eq(const char *a, const char *b)
{
	for (int i = 0; i < 16; i++) {
		char ca = a[i], cb = b[i];
		if (ca >= 'a' && ca <= 'z') ca -= 32;
		if (cb >= 'a' && cb <= 'z') cb -= 32;
		if (ca != cb) return 0;
		if (ca == 0)  return 1;
	}
	return 1;
}

static int stinkfs_find(struct fs_mount *m, const char *name)
{
	struct fs_dir *d = mount_dir(m);
	for (unsigned int i = 0; i < d->count; i++)
		if (name_eq(d->files[i].name, name))
			return (int)i;
	return -1;
}

static unsigned int need_sectors(unsigned int size)
{
	return (size + 511) / 512;
}

static int stinkfs_file_write(struct fs_mount *m, const char *name,
                              const void *buf, unsigned int size)
{
	struct fs_dir *d = mount_dir(m);
	unsigned int need = need_sectors(size);
	int i = stinkfs_find(m, name);
	unsigned int start;

	if (i >= 0 && need <= need_sectors(d->files[i].size)) {
		start = d->files[i].start;
	} else {
		if (d->next_free + need > m->data_end)
			return -1;
		start = d->next_free;
		d->next_free += need;
		if (i < 0) {
			if (d->count >= FS_MAX_FILES)
				return -1;
			i = (int)d->count;
			d->count++;
			for (int k = 0; k < 16; k++)
				d->files[i].name[k] = name[k];
		}
	}

	d->files[i].start = start;
	d->files[i].size  = size;

	unsigned int remaining = size;
	unsigned int lba = start;
	const unsigned char *src = (const unsigned char *)buf;
	while (remaining > 0) {
		unsigned int chunk = remaining < 512 ? remaining : 512;
		for (int k = 0; k < 512; k++)
			m->io_buf[k] = 0;
		for (unsigned int k = 0; k < chunk; k++)
			m->io_buf[k] = src[k];
		if (ata_drive_write(m->drive, lba, 1, m->io_buf) != 0)
			return -1;
		src += chunk;
		remaining -= chunk;
		lba++;
	}

	return stinkfs_dir_save(m);
}

static int stinkfs_file_read_at(struct fs_mount *m, const char *name, void *buf,
                                unsigned int maxsize, unsigned int offset)
{
	struct fs_dir *d = mount_dir(m);
	int i = stinkfs_find(m, name);
	if (i < 0) return -1;

	unsigned int size = d->files[i].size;
	if (offset >= size) return 0;

	unsigned int avail = size - offset;
	unsigned int n = avail < maxsize ? avail : maxsize;

	unsigned int remaining = n;
	unsigned int pos = offset;
	unsigned char *dst = (unsigned char *)buf;
	while (remaining > 0) {
		unsigned int sec = d->files[i].start + pos / 512;
		unsigned int off = pos % 512;
		unsigned int chunk = 512 - off;
		if (chunk > remaining) chunk = remaining;
		if (ata_drive_read(m->drive, sec, 1, m->io_buf) != 0)
			return -1;
		for (unsigned int k = 0; k < chunk; k++)
			dst[k] = m->io_buf[off + k];
		dst += chunk;
		remaining -= chunk;
		pos += chunk;
	}
	return (int)n;
}

static int stinkfs_file_delete(struct fs_mount *m, const char *name)
{
	struct fs_dir *d = mount_dir(m);
	int i = stinkfs_find(m, name);
	if (i < 0) return -1;

	unsigned int freed = need_sectors(d->files[i].size);
	unsigned int hole  = d->files[i].start;

	for (unsigned int lba = hole + freed; lba < d->next_free; lba++) {
		if (ata_drive_read(m->drive, lba, 1, m->io_buf) != 0)
			return -1;
		if (ata_drive_write(m->drive, lba - freed, 1, m->io_buf) != 0)
			return -1;
	}

	for (unsigned int j = 0; j < d->count; j++)
		if (d->files[j].start > hole)
			d->files[j].start -= freed;

	for (unsigned int j = (unsigned int)i; j + 1 < d->count; j++)
		d->files[j] = d->files[j + 1];
	d->count--;
	d->next_free -= freed;

	return stinkfs_dir_save(m);
}

static int stinkfs_grow(struct fs_mount *m, int i, unsigned int new_size)
{
	struct fs_dir *d = mount_dir(m);
	unsigned int start    = d->files[i].start;
	unsigned int old_sect = need_sectors(d->files[i].size);
	unsigned int new_sect = need_sectors(new_size);

	if (new_sect <= old_sect) return 0;

	if (start + old_sect == d->next_free) {
		if (start + new_sect > m->data_end) return -1;
		d->next_free = start + new_sect;
		return 0;
	}

	if (d->next_free + new_sect > m->data_end) return -1;
	unsigned int dst = d->next_free;
	for (unsigned int s = 0; s < old_sect; s++) {
		if (ata_drive_read(m->drive, start + s, 1, m->io_buf) != 0)
			return -1;
		if (ata_drive_write(m->drive, dst + s, 1, m->io_buf) != 0)
			return -1;
	}
	d->next_free += new_sect;
	d->files[i].start = dst;
	return 0;
}

static int stinkfs_put(struct fs_mount *m, unsigned int start, unsigned int pos,
                       const void *buf, unsigned int size)
{
	unsigned int remaining = size;
	const unsigned char *src = (const unsigned char *)buf;
	while (remaining > 0) {
		unsigned int sec = start + pos / 512;
		unsigned int off = pos % 512;
		unsigned int chunk = 512 - off;
		if (chunk > remaining) chunk = remaining;
		if (ata_drive_read(m->drive, sec, 1, m->io_buf) != 0)
			return -1;
		for (unsigned int k = 0; k < chunk; k++)
			m->io_buf[off + k] = src[k];
		if (ata_drive_write(m->drive, sec, 1, m->io_buf) != 0)
			return -1;
		pos += chunk;
		remaining -= chunk;
		src += chunk;
	}
	return 0;
}

static int stinkfs_file_append(struct fs_mount *m, const char *name,
                               const void *buf, unsigned int size)
{
	struct fs_dir *d = mount_dir(m);
	int i = stinkfs_find(m, name);
	if (i < 0) return stinkfs_file_write(m, name, buf, size);
	if (size == 0) return 0;

	unsigned int old_size = d->files[i].size;
	unsigned int new_size = old_size + size;
	if (new_size < old_size) return -1;
	if (stinkfs_grow(m, i, new_size) != 0) return -1;

	if (stinkfs_put(m, d->files[i].start, old_size, buf, size) != 0)
		return -1;
	d->files[i].size = new_size;
	return stinkfs_dir_save(m);
}

static int stinkfs_file_write_at(struct fs_mount *m, const char *name,
                                 const void *buf, unsigned int size,
                                 unsigned int offset)
{
	struct fs_dir *d = mount_dir(m);
	if (size == 0) return 0;

	int i = stinkfs_find(m, name);
	if (i < 0)
		return offset == 0 ? stinkfs_file_write(m, name, buf, size) : -1;

	unsigned int old_size = d->files[i].size;
	if (offset > old_size) return -1;

	unsigned int end = offset + size;
	if (end < offset) return -1;

	if (end > old_size) {
		if (stinkfs_grow(m, i, end) != 0) return -1;
		d->files[i].size = end;
	}

	if (stinkfs_put(m, d->files[i].start, offset, buf, size) != 0)
		return -1;
	return stinkfs_dir_save(m);
}

static int stinkfs_file_count(struct fs_mount *m)
{
	return (int)mount_dir(m)->count;
}

static int stinkfs_file_size(struct fs_mount *m, const char *name)
{
	int i = stinkfs_find(m, name);
	return i < 0 ? -1 : (int)mount_dir(m)->files[i].size;
}

static int stinkfs_file_info(struct fs_mount *m, int index, char *name_out)
{
	struct fs_dir *d = mount_dir(m);
	if (index < 0 || (unsigned int)index >= d->count) return -1;
	for (int k = 0; k < 16; k++)
		name_out[k] = d->files[index].name[k];
	return (int)d->files[index].size;
}

static int stinkfs_file_lba_sectors(struct fs_mount *m, const char *name,
                                    unsigned int *lba_out,
                                    unsigned int *sectors_out)
{
	struct fs_dir *d = mount_dir(m);
	for (unsigned int i = 0; i < d->count; i++) {
		if (name_ci_eq(d->files[i].name, name)) {
			*lba_out     = d->files[i].start;
			*sectors_out = need_sectors(d->files[i].size);
			return 0;
		}
	}
	return -1;
}

static const struct fs_ops stinkfs_ops = {
	.file_write       = stinkfs_file_write,
	.file_write_at    = stinkfs_file_write_at,
	.file_append      = stinkfs_file_append,
	.file_read_at     = stinkfs_file_read_at,
	.file_delete      = stinkfs_file_delete,
	.file_count       = stinkfs_file_count,
	.file_info        = stinkfs_file_info,
	.file_size        = stinkfs_file_size,
	.file_lba_sectors = stinkfs_file_lba_sectors,
};

/* ---- mount table glue ---- */

static struct fs_mount *active_mount(void)
{
	return mounts[0].present ? &mounts[0] : 0;
}

/* Strip an optional "X:" DOS-style mount prefix off the front of a
 * filename and return the resolved mount slot. X = A..H (case-insens)
 * indexes mounts[X - 'A']. Without a prefix, defaults to slot 0.
 * Returns 0 (NULL mount) when the slot is unregistered, letting the
 * caller fall through to the standard "no such file" error path. */
static struct fs_mount *resolve(const char *name_in, const char **name_out)
{
	char c = name_in[0];
	if (name_in[1] == ':' && ((c >= 'A' && c <= 'H') ||
	                          (c >= 'a' && c <= 'h'))) {
		int slot = (c >= 'a') ? (c - 'a') : (c - 'A');
		if (slot < 0 || slot >= FS_MAX_MOUNTS || !mounts[slot].present)
			return 0;
		*name_out = name_in + 2;
		return &mounts[slot];
	}
	*name_out = name_in;
	return active_mount();
}

int fs_init(void)
{
	mounts[0].present  = 1;
	mounts[0].drive    = 0;
	mounts[0].dir_lba  = FS_DIR_LBA;
	mounts[0].data_lba = FS_DATA_LBA;
	mounts[0].data_end = FS_DATA_END;
	mounts[0].ops      = &stinkfs_ops;
	return stinkfs_dir_load(&mounts[0]);
}

/* ---- public API: prefix-routed (A:/B:/...) with slot 0 default ---- */

int fs_file_write(const char *name, const void *buf, unsigned int size)
{
	const char *rest;
	struct fs_mount *m = resolve(name, &rest);
	return m ? m->ops->file_write(m, rest, buf, size) : -1;
}

int fs_file_write_at(const char *name, const void *buf, unsigned int size,
                     unsigned int offset)
{
	const char *rest;
	struct fs_mount *m = resolve(name, &rest);
	return m ? m->ops->file_write_at(m, rest, buf, size, offset) : -1;
}

int fs_file_append(const char *name, const void *buf, unsigned int size)
{
	const char *rest;
	struct fs_mount *m = resolve(name, &rest);
	return m ? m->ops->file_append(m, rest, buf, size) : -1;
}

int fs_file_read(const char *name, void *buf, unsigned int maxsize)
{
	const char *rest;
	struct fs_mount *m = resolve(name, &rest);
	return m ? m->ops->file_read_at(m, rest, buf, maxsize, 0) : -1;
}

int fs_file_read_at(const char *name, void *buf, unsigned int maxsize,
                    unsigned int offset)
{
	const char *rest;
	struct fs_mount *m = resolve(name, &rest);
	return m ? m->ops->file_read_at(m, rest, buf, maxsize, offset) : -1;
}

int fs_file_delete(const char *name)
{
	const char *rest;
	struct fs_mount *m = resolve(name, &rest);
	return m ? m->ops->file_delete(m, rest) : -1;
}

/* fs_file_count + fs_file_info enumerate slot 0 only; menu/ls have
 * never needed cross-mount enumeration. A later API can add a slot
 * argument when a real consumer appears. */
int fs_file_count(void)
{
	struct fs_mount *m = active_mount();
	return m ? m->ops->file_count(m) : 0;
}

int fs_file_info(int index, char *name_out)
{
	struct fs_mount *m = active_mount();
	return m ? m->ops->file_info(m, index, name_out) : -1;
}

int fs_file_size(const char *name)
{
	const char *rest;
	struct fs_mount *m = resolve(name, &rest);
	return m ? m->ops->file_size(m, rest) : -1;
}

int fs_file_lba_sectors(const char *name, unsigned int *lba_out,
                        unsigned int *sectors_out)
{
	const char *rest;
	struct fs_mount *m = resolve(name, &rest);
	return m ? m->ops->file_lba_sectors(m, rest, lba_out, sectors_out) : -1;
}
