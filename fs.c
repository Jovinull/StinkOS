/* Reads the app table-of-contents the build wrote to a fixed disk sector, so the
 * menu and loader discover apps from disk instead of a hard-coded list. */
#include "fs.h"
#include "ata.h"

#define TOC_LBA   120              /* must match the Makefile */
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
