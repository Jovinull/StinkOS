/* On-disk storage for StinkOS.
 *
 * StinkFS: a small writable filesystem of named files. The directory spans two
 * contiguous sectors at FS_DIR_LBA, followed by a contiguous data region at
 * FS_DATA_LBA. All app ELFs, WAD assets, and persistent userland state live
 * here as named files.
 *
 * Disk layout (must match the Makefile):
 *   LBA 0     : boot sector
 *   LBA 1-127 : kernel (127 sectors, up to 63.5 KiB)
 *   LBA 128-129 : StinkFS directory (2 sectors)
 *   LBA 130+  : StinkFS data (~100 MiB) */
#include "fs.h"
#include "ata.h"

#define STINKFS_MAGIC 0x4B4E5453u  /* 'S','T','N','K' little-endian */
#define FS_DIR_LBA    512          /* first directory sector            */
#define FS_DIR_SECTORS 2           /* directory spans two 512-byte sectors */
#define FS_DATA_LBA   514          /* first data sector                 */
#define FS_DATA_END   200514       /* one past the last data sector (~100 MiB) */
#define FS_MAX_FILES  40           /* fits in 2 sectors: 12 + 40*24 = 972 B  */

struct fs_file {
	char         name[16];
	unsigned int start;        /* first data sector (absolute LBA) */
	unsigned int size;         /* file length in bytes */
} __attribute__((packed));

struct fs_dir {
	unsigned int   magic;
	unsigned int   count;
	unsigned int   next_free;  /* next unallocated data sector */
	struct fs_file files[FS_MAX_FILES];
} __attribute__((packed));

/* Directory buffer: two 512-byte sectors. */
static unsigned char dir_buf[512 * FS_DIR_SECTORS];
static unsigned char io_buf[512];          /* per-sector bounce buffer */
static struct fs_dir *dir = (struct fs_dir *)dir_buf;

static int fs_dir_load(void)
{
	if (ata_read(FS_DIR_LBA, FS_DIR_SECTORS, dir_buf) != 0)
		return -1;                         /* disk read failed: do not guess  */
	if (dir->magic != STINKFS_MAGIC) {     /* uninitialised: start empty       */
		dir->magic     = STINKFS_MAGIC;
		dir->count     = 0;
		dir->next_free = FS_DATA_LBA;
	}
	return 0;
}

static int fs_dir_save(void)
{
	return ata_write(FS_DIR_LBA, FS_DIR_SECTORS, dir_buf);
}

int fs_init(void)
{
	return fs_dir_load();
}

/* ---- name helpers ---- */

static int name_eq(const char *a, const char *b)
{
	for (int i = 0; i < 16; i++)
		if (a[i] != b[i])
			return 0;
	return 1;
}

static int name_ci_eq(const char *a, const char *b)
{
	for (int i = 0; i < 16; i++) {
		char ca = a[i], cb = b[i];
		if (ca >= 'a' && ca <= 'z') ca -= 32;
		if (cb >= 'a' && cb <= 'z') cb -= 32;
		if (ca != cb) return 0;
	}
	return 1;
}

static int fs_find(const char *name)
{
	for (unsigned int i = 0; i < dir->count; i++)
		if (name_eq(dir->files[i].name, name))
			return (int)i;
	return -1;
}

static unsigned int need_sectors(unsigned int size)
{
	return (size + 511) / 512;
}

/* ---- public API ---- */

/* Writes 'size' bytes from 'buf' to the file 'name', creating it or reusing
 * its existing region when the new size still fits. */
int fs_file_write(const char *name, const void *buf, unsigned int size)
{
	unsigned int need = need_sectors(size);
	int i = fs_find(name);
	unsigned int start;

	if (i >= 0 && need <= need_sectors(dir->files[i].size)) {
		start = dir->files[i].start;       /* reuse the current region */
	} else {
		if (dir->next_free + need > FS_DATA_END)
			return -1;
		start = dir->next_free;
		dir->next_free += need;
		if (i < 0) {
			if (dir->count >= FS_MAX_FILES)
				return -1;
			i = (int)dir->count;
			dir->count++;
			for (int k = 0; k < 16; k++)
				dir->files[i].name[k] = name[k];
		}
	}

	dir->files[i].start = start;
	dir->files[i].size  = size;

	unsigned int remaining = size;
	unsigned int lba = start;
	const unsigned char *src = (const unsigned char *)buf;
	while (remaining > 0) {
		unsigned int chunk = remaining < 512 ? remaining : 512;
		for (int k = 0; k < 512; k++)
			io_buf[k] = 0;
		for (unsigned int k = 0; k < chunk; k++)
			io_buf[k] = src[k];
		if (ata_write(lba, 1, io_buf) != 0)
			return -1;
		src += chunk;
		remaining -= chunk;
		lba++;
	}

	return fs_dir_save();
}

/* Reads up to 'maxsize' bytes of file 'name' starting at byte 'offset'. */
int fs_file_read_at(const char *name, void *buf, unsigned int maxsize,
                    unsigned int offset)
{
	int i = fs_find(name);
	if (i < 0)
		return -1;

	unsigned int size = dir->files[i].size;
	if (offset >= size)
		return 0;

	unsigned int avail = size - offset;
	unsigned int n = avail < maxsize ? avail : maxsize;

	unsigned int remaining = n;
	unsigned int pos = offset;
	unsigned char *dst = (unsigned char *)buf;
	while (remaining > 0) {
		unsigned int sec = dir->files[i].start + pos / 512;
		unsigned int off = pos % 512;
		unsigned int chunk = 512 - off;
		if (chunk > remaining)
			chunk = remaining;
		if (ata_read(sec, 1, io_buf) != 0)
			return -1;
		for (unsigned int k = 0; k < chunk; k++)
			dst[k] = io_buf[off + k];
		dst += chunk;
		remaining -= chunk;
		pos += chunk;
	}
	return (int)n;
}

int fs_file_read(const char *name, void *buf, unsigned int maxsize)
{
	return fs_file_read_at(name, buf, maxsize, 0);
}

/* Deletes file 'name', compacting the data region. */
int fs_file_delete(const char *name)
{
	int i = fs_find(name);
	if (i < 0)
		return -1;

	unsigned int freed = need_sectors(dir->files[i].size);
	unsigned int hole  = dir->files[i].start;

	for (unsigned int lba = hole + freed; lba < dir->next_free; lba++) {
		if (ata_read(lba, 1, io_buf) != 0)
			return -1;
		if (ata_write(lba - freed, 1, io_buf) != 0)
			return -1;
	}

	for (unsigned int j = 0; j < dir->count; j++)
		if (dir->files[j].start > hole)
			dir->files[j].start -= freed;

	for (unsigned int j = (unsigned int)i; j + 1 < dir->count; j++)
		dir->files[j] = dir->files[j + 1];
	dir->count--;
	dir->next_free -= freed;

	return fs_dir_save();
}

static int fs_grow(int i, unsigned int new_size)
{
	unsigned int start    = dir->files[i].start;
	unsigned int old_sect = need_sectors(dir->files[i].size);
	unsigned int new_sect = need_sectors(new_size);

	if (new_sect <= old_sect)
		return 0;

	if (start + old_sect == dir->next_free) {
		if (start + new_sect > FS_DATA_END)
			return -1;
		dir->next_free = start + new_sect;
		return 0;
	}

	if (dir->next_free + new_sect > FS_DATA_END)
		return -1;
	unsigned int dst = dir->next_free;
	for (unsigned int s = 0; s < old_sect; s++) {
		if (ata_read(start + s, 1, io_buf) != 0)
			return -1;
		if (ata_write(dst + s, 1, io_buf) != 0)
			return -1;
	}
	dir->next_free += new_sect;
	dir->files[i].start = dst;
	return 0;
}

static int fs_put(unsigned int start, unsigned int pos,
                  const void *buf, unsigned int size)
{
	unsigned int remaining = size;
	const unsigned char *src = (const unsigned char *)buf;
	while (remaining > 0) {
		unsigned int sec = start + pos / 512;
		unsigned int off = pos % 512;
		unsigned int chunk = 512 - off;
		if (chunk > remaining)
			chunk = remaining;
		if (ata_read(sec, 1, io_buf) != 0)
			return -1;
		for (unsigned int k = 0; k < chunk; k++)
			io_buf[off + k] = src[k];
		if (ata_write(sec, 1, io_buf) != 0)
			return -1;
		pos += chunk;
		remaining -= chunk;
		src += chunk;
	}
	return 0;
}

int fs_file_append(const char *name, const void *buf, unsigned int size)
{
	int i = fs_find(name);
	if (i < 0)
		return fs_file_write(name, buf, size);
	if (size == 0)
		return 0;

	unsigned int old_size = dir->files[i].size;
	unsigned int new_size = old_size + size;
	if (new_size < old_size)
		return -1;
	if (fs_grow(i, new_size) != 0)
		return -1;

	if (fs_put(dir->files[i].start, old_size, buf, size) != 0)
		return -1;
	dir->files[i].size = new_size;
	return fs_dir_save();
}

int fs_file_write_at(const char *name, const void *buf, unsigned int size,
                     unsigned int offset)
{
	if (size == 0)
		return 0;

	int i = fs_find(name);
	if (i < 0)
		return offset == 0 ? fs_file_write(name, buf, size) : -1;

	unsigned int old_size = dir->files[i].size;
	if (offset > old_size)
		return -1;

	unsigned int end = offset + size;
	if (end < offset)
		return -1;

	if (end > old_size) {
		if (fs_grow(i, end) != 0)
			return -1;
		dir->files[i].size = end;
	}

	if (fs_put(dir->files[i].start, offset, buf, size) != 0)
		return -1;
	return fs_dir_save();
}

int fs_file_count(void)
{
	return (int)dir->count;
}

int fs_file_size(const char *name)
{
	int i = fs_find(name);
	return i < 0 ? -1 : (int)dir->files[i].size;
}

int fs_file_info(int index, char *name_out)
{
	if (index < 0 || (unsigned int)index >= dir->count)
		return -1;
	for (int k = 0; k < 16; k++)
		name_out[k] = dir->files[index].name[k];
	return (int)dir->files[index].size;
}

/* Returns the absolute LBA and sector count of file 'name' (case-insensitive).
 * Used by the ELF loader to launch apps by filename from the menu and shell. */
int fs_file_lba_sectors(const char *name, unsigned int *lba_out,
                        unsigned int *sectors_out)
{
	for (unsigned int i = 0; i < dir->count; i++) {
		if (name_ci_eq(dir->files[i].name, name)) {
			*lba_out     = dir->files[i].start;
			*sectors_out = need_sectors(dir->files[i].size);
			return 0;
		}
	}
	return -1;
}
