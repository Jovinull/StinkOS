/* On-disk storage for StinkOS. Holds three things, all matching the Makefile
 * layout: the read-only app table-of-contents (TOC) the menu boots from, a
 * single-value scratch sector, and StinkFS -- a small writable filesystem of
 * named files (a one-sector directory plus a contiguous data region). */
#include "fs.h"
#include "ata.h"

#define TOC_LBA   136              /* must match the Makefile */
#define SAVE_LBA  137              /* one sector of persistent userland storage */
#define MAX_APPS  16

/* StinkFS layout. */
#define STINKFS_MAGIC 0x4B4E5453u  /* 'S','T','N','K' little-endian */
#define FS_DIR_LBA    138          /* directory sector */
#define FS_DATA_LBA   139          /* first data sector */
#define FS_DATA_END   171          /* one past the last data sector (32 sectors) */
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

/* Persistent storage: one disk sector holding a single 32-bit value, written
 * and read back through the ATA driver so it survives across app launches. */
static unsigned char save_buf[512];

unsigned int fs_load(void)
{
	ata_read(SAVE_LBA, 1, save_buf);
	return *(unsigned int *)save_buf;
}

void fs_save(unsigned int value)
{
	*(unsigned int *)save_buf = value;
	ata_write(SAVE_LBA, 1, save_buf);
}

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

/* Reads up to 'maxsize' bytes of file 'name' into 'buf'. Returns the number of
 * bytes copied, or -1 if the file does not exist. */
int fs_file_read(const char *name, void *buf, unsigned int maxsize)
{
	int i = fs_find(name);
	if (i < 0)
		return -1;

	unsigned int n = dir->files[i].size;
	if (n > maxsize)
		n = maxsize;

	unsigned int remaining = n;
	unsigned int lba = dir->files[i].start;
	unsigned char *dst = (unsigned char *)buf;
	while (remaining > 0) {
		ata_read(lba, 1, io_buf);
		unsigned int chunk = remaining < 512 ? remaining : 512;
		for (unsigned int k = 0; k < chunk; k++)
			dst[k] = io_buf[k];
		dst += chunk;
		remaining -= chunk;
		lba++;
	}
	return (int)n;
}
