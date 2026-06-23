/* ATA PIO read on the primary bus (I/O base 0x1F0), drive 0, LBA28 polling.
 * Enough to pull app binaries from fixed raw sectors until a real FS exists. */
#include "ata.h"
#include "io.h"

#define ATA_DATA     0x1F0
#define ATA_SECCOUNT 0x1F2
#define ATA_LBA_LO   0x1F3
#define ATA_LBA_MID  0x1F4
#define ATA_LBA_HI   0x1F5
#define ATA_DRIVE    0x1F6
#define ATA_CMD      0x1F7      /* write: command  / read: status */

#define ST_BSY 0x80
#define ST_DRQ 0x08

#define CMD_READ_SECTORS  0x20
#define CMD_WRITE_SECTORS 0x30
#define CMD_FLUSH_CACHE   0xE7

static void ata_poll(void)
{
	while (inb(ATA_CMD) & ST_BSY)
		;
	while (!(inb(ATA_CMD) & ST_DRQ))
		;
}

static void ata_select(unsigned int lba, unsigned int count, unsigned char cmd)
{
	while (inb(ATA_CMD) & ST_BSY)
		;

	outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));   /* master, LBA mode */
	outb(ATA_SECCOUNT, count);
	outb(ATA_LBA_LO, lba & 0xFF);
	outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
	outb(ATA_LBA_HI, (lba >> 16) & 0xFF);
	outb(ATA_CMD, cmd);
}

void ata_read(unsigned int lba, unsigned int count, void *buffer)
{
	unsigned short *buf = (unsigned short *)buffer;

	ata_select(lba, count, CMD_READ_SECTORS);

	for (unsigned int s = 0; s < count; s++) {
		ata_poll();
		for (int i = 0; i < 256; i++)           /* 256 words = 512 bytes */
			buf[i] = inw(ATA_DATA);
		buf += 256;
	}
}

void ata_write(unsigned int lba, unsigned int count, const void *buffer)
{
	const unsigned short *buf = (const unsigned short *)buffer;

	ata_select(lba, count, CMD_WRITE_SECTORS);

	for (unsigned int s = 0; s < count; s++) {
		ata_poll();
		for (int i = 0; i < 256; i++)           /* 256 words = 512 bytes */
			outw(ATA_DATA, buf[i]);
		buf += 256;
	}

	outb(ATA_CMD, CMD_FLUSH_CACHE);                 /* commit to the medium */
	while (inb(ATA_CMD) & ST_BSY)
		;
}
