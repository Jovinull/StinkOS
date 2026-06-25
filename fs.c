/* On-disk storage for StinkOS. Holds two things, both matching the Makefile
 * layout: the read-only app table-of-contents (TOC) the menu boots from, and
 * StinkFS -- a small writable filesystem of named files (a one-sector directory
 * plus a contiguous data region). All persistent userland state lives in files. */
#include "fs.h"
#include "ata.h"

/* Disk layout (must match the Makefile). The app region spans LBA 64..223;
 * apps are placed at the LBAs recorded in the TOC (slot sizes need not be
 * uniform), and all metadata lives above the region. */
#define TOC_LBA   224              /* app table-of-contents */
#define MAX_APPS  20

/* StinkFS layout. */
#define STINKFS_MAGIC 0x4B4E5453u  /* 'S','T','N','K' little-endian */
#define FS_DIR_LBA    225          /* directory sector */
#define FS_DATA_LBA   226          /* first data sector */
#define FS_DATA_END   258          /* one past the last data sector (32 sectors) */
#define FS_MAX_FILES  16

struct toc_entry {
	char         name[16];
	unsigned int lba;
	unsigned int sectors;
} __attribute__((packed));

static unsigned char buf[512];
static int           count;
static struct toc_entry *entries;

/* StinkFS directory: a magic word, a file count, a bump pointer to the next
 * free data sector, then a fixed table of name/start/size records. The whole
 * structure lives in one disk sector. */
struct fs_file {
	char         name[16];
	unsigned int start;        /* first data sector */
	unsigned int size;         /* file length in bytes */
} __attribute__((packed));

struct fs_dir {
	unsigned int   magic;
	unsigned int   count;
	unsigned int   next_free;  /* next unallocated data sector */
	struct fs_file files[FS_MAX_FILES];
} __attribute__((packed));

static unsigned char dir_buf[512];
static unsigned char io_buf[512];          /* per-sector bounce buffer */
static struct fs_dir *dir = (struct fs_dir *)dir_buf;

static void fs_dir_load(void)
{
	ata_read(FS_DIR_LBA, 1, dir_buf);
	if (dir->magic != STINKFS_MAGIC) {     /* uninitialised: start empty */
		dir->magic     = STINKFS_MAGIC;
		dir->count     = 0;
		dir->next_free = FS_DATA_LBA;
	}
}

void fs_init(void)
{
	ata_read(TOC_LBA, 1, buf);
	count = *(unsigned int *)buf;
	if (count < 0 || count > MAX_APPS)
		count = 0;
	entries = (struct toc_entry *)(buf + 4);

	fs_dir_load();
}

int          fs_count(void)            { return count; }
const char  *fs_name(int index)        { return entries[index].name; }
unsigned int fs_lba(int index)         { return entries[index].lba; }
unsigned int fs_sectors(int index)     { return entries[index].sectors; }

/* ---- StinkFS named files ---- */

/* Names are compared and stored as a fixed 16-byte field; the caller passes a
 * NUL-padded canonical name (see the syscall layer). */
static int name_eq(const char *a, const char *b)
{
	for (int i = 0; i < 16; i++)
		if (a[i] != b[i])
			return 0;
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

/* Writes 'size' bytes from 'buf' to the file 'name', creating it or reusing its
 * existing region when the new size still fits. Returns 0 on success, -1 if the
 * directory is full or the data region has no room. */
int fs_file_write(const char *name, const void *buf, unsigned int size)
{
	unsigned int need = need_sectors(size);
	int i = fs_find(name);
	unsigned int start;

	if (i >= 0 && need <= need_sectors(dir->files[i].size)) {
		start = dir->files[i].start;       /* reuse the current region */
	} else {
		if (dir->next_free + need > FS_DATA_END)
			return -1;                 /* out of data space */
		start = dir->next_free;
		dir->next_free += need;
		if (i < 0) {                       /* new directory entry */
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
			io_buf[k] = 0;             /* zero-pad the tail sector */
		for (unsigned int k = 0; k < chunk; k++)
			io_buf[k] = src[k];
		ata_write(lba, 1, io_buf);
		src += chunk;
		remaining -= chunk;
		lba++;
	}

	ata_write(FS_DIR_LBA, 1, dir_buf);         /* persist the directory */
	return 0;
}

/* Reads up to 'maxsize' bytes of file 'name' starting at byte 'offset'. Returns
 * the number of bytes copied (0 if offset is at/after EOF), or -1 if the file
 * does not exist. */
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
		ata_read(sec, 1, io_buf);
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

/* Deletes file 'name', compacting the data region so no space is leaked: every
 * file stored after the hole is shifted down over it, its start sector fixed up,
 * and the bump pointer rewound. Returns 0 on success, -1 if not found. */
int fs_file_delete(const char *name)
{
	int i = fs_find(name);
	if (i < 0)
		return -1;

	unsigned int freed = need_sectors(dir->files[i].size);
	unsigned int hole  = dir->files[i].start;

	/* Slide the data of every later file down over the freed sectors. Source
	 * sits above the destination, so an ascending copy never self-overwrites. */
	for (unsigned int lba = hole + freed; lba < dir->next_free; lba++) {
		ata_read(lba, 1, io_buf);
		ata_write(lba - freed, 1, io_buf);
	}

	for (unsigned int j = 0; j < dir->count; j++)
		if (dir->files[j].start > hole)
			dir->files[j].start -= freed;

	for (unsigned int j = (unsigned int)i; j + 1 < dir->count; j++)
		dir->files[j] = dir->files[j + 1];
	dir->count--;
	dir->next_free -= freed;

	ata_write(FS_DIR_LBA, 1, dir_buf);
	return 0;
}

/* Ensures file 'i' has room for at least 'new_size' bytes, growing its region
 * when needed: extend the bump pointer if it is the last file, else relocate it
 * to fresh space (copying the old sectors). Updates start. Returns 0 or -1. */
static int fs_grow(int i, unsigned int new_size)
{
	unsigned int start    = dir->files[i].start;
	unsigned int old_sect = need_sectors(dir->files[i].size);
	unsigned int new_sect = need_sectors(new_size);

	if (new_sect <= old_sect)
		return 0;

	if (start + old_sect == dir->next_free) {          /* last file: extend */
		if (start + new_sect > FS_DATA_END)
			return -1;
		dir->next_free = start + new_sect;
		return 0;
	}

	if (dir->next_free + new_sect > FS_DATA_END)        /* relocate with room */
		return -1;
	unsigned int dst = dir->next_free;
	for (unsigned int s = 0; s < old_sect; s++) {
		ata_read(start + s, 1, io_buf);
		ata_write(dst + s, 1, io_buf);
	}
	dir->next_free += new_sect;
	dir->files[i].start = dst;
	return 0;
}

/* Writes 'size' bytes from 'buf' into the data region at byte 'pos' (relative to
 * 'start'), read-modify-writing each touched sector so neighbouring bytes are
 * preserved. The region must already be allocated (see fs_grow). */
static void fs_put(unsigned int start, unsigned int pos,
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
		ata_read(sec, 1, io_buf);                  /* preserve other bytes */
		for (unsigned int k = 0; k < chunk; k++)
			io_buf[off + k] = src[k];
		ata_write(sec, 1, io_buf);
		pos += chunk;
		remaining -= chunk;
		src += chunk;
	}
}

/* Appends 'size' bytes to file 'name' (creating it if absent). */
int fs_file_append(const char *name, const void *buf, unsigned int size)
{
	int i = fs_find(name);
	if (i < 0)
		return fs_file_write(name, buf, size);
	if (size == 0)
		return 0;

	unsigned int old_size = dir->files[i].size;
	unsigned int new_size = old_size + size;
	if (new_size < old_size)                           /* size overflow */
		return -1;
	if (fs_grow(i, new_size) != 0)
		return -1;

	fs_put(dir->files[i].start, old_size, buf, size);
	dir->files[i].size = new_size;
	ata_write(FS_DIR_LBA, 1, dir_buf);
	return 0;
}

/* Writes 'size' bytes to file 'name' starting at byte 'offset', overwriting in
 * place and extending the file when the write runs past its end. 'offset' may
 * not exceed the current size (no sparse holes). Creates the file when absent
 * and offset is 0. Returns 0 on success, -1 on error. */
int fs_file_write_at(const char *name, const void *buf, unsigned int size,
                     unsigned int offset)
{
	if (size == 0)
		return 0;

	int i = fs_find(name);
	if (i < 0)
		return offset == 0 ? fs_file_write(name, buf, size) : -1;

	unsigned int old_size = dir->files[i].size;
	if (offset > old_size)                             /* would leave a hole */
		return -1;

	unsigned int end = offset + size;
	if (end < offset)                                  /* overflow */
		return -1;

	if (end > old_size) {
		if (fs_grow(i, end) != 0)
			return -1;
		dir->files[i].size = end;
	}

	fs_put(dir->files[i].start, offset, buf, size);
	ata_write(FS_DIR_LBA, 1, dir_buf);
	return 0;
}

int fs_file_count(void)
{
	return (int)dir->count;
}

/* Returns the size in bytes of file 'name', or -1 if it does not exist. */
int fs_file_size(const char *name)
{
	int i = fs_find(name);
	return i < 0 ? -1 : (int)dir->files[i].size;
}

/* Copies the 16-byte name of file 'index' into 'name_out' and returns its size
 * in bytes, or -1 if the index is out of range. */
int fs_file_info(int index, char *name_out)
{
	if (index < 0 || (unsigned int)index >= dir->count)
		return -1;
	for (int k = 0; k < 16; k++)
		name_out[k] = dir->files[index].name[k];
	return (int)dir->files[index].size;
}
