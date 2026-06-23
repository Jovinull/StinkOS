/* Reads the app table-of-contents the build wrote to a fixed disk sector, so the
 * menu and loader discover apps from disk instead of a hard-coded list. */
#include "fs.h"
#include "ata.h"

#define TOC_LBA   128              /* must match the Makefile */
#define SAVE_LBA  129              /* one sector of persistent userland storage */
#define MAX_APPS  16

struct toc_entry {
	char         name[16];
	unsigned int lba;
	unsigned int sectors;
} __attribute__((packed));

static unsigned char buf[512];
static int           count;
static struct toc_entry *entries;

void fs_init(void)
{
	ata_read(TOC_LBA, 1, buf);
	count = *(unsigned int *)buf;
	if (count < 0 || count > MAX_APPS)
		count = 0;
	entries = (struct toc_entry *)(buf + 4);
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
