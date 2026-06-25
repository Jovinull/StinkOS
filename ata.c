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

#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_BSY 0x80

#define CMD_READ_SECTORS  0x20
#define CMD_WRITE_SECTORS 0x30
#define CMD_FLUSH_CACHE   0xE7
#define CMD_IDENTIFY      0xEC

/* A real drive answers in microseconds; this many spins is already a very
 * generous upper bound. Without it, a missing/faulty drive would hang the
 * whole kernel in a busy-loop that never exits instead of just failing the
 * read/write. */
#define ATA_TIMEOUT_SPINS 1000000

static int ata_wait_ready(void)
{
	unsigned int spins = 0;
	while ((inb(ATA_CMD) & ST_BSY) && spins < ATA_TIMEOUT_SPINS)
		spins++;
	return (spins < ATA_TIMEOUT_SPINS) ? 0 : -1;
}

/* Waits for the drive to clear BSY and then either flag an error or assert
 * DRQ (data ready to transfer). Returns 0 if it's safe to move data, -1 on
 * a reported error or a timeout. */
static int ata_poll(void)
{
	if (ata_wait_ready() != 0)
		return -1;

	unsigned int spins = 0;
	for (;;) {
		unsigned char status = inb(ATA_CMD);
		if (status & ST_ERR)
			return -1;
		if (status & ST_DRQ)
			return 0;
		if (++spins >= ATA_TIMEOUT_SPINS)
			return -1;
	}
}

static int ata_select(unsigned int lba, unsigned int count, unsigned char cmd)
{
	if (ata_wait_ready() != 0)
		return -1;

	outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));   /* master, LBA mode */
	outb(ATA_SECCOUNT, count);
	outb(ATA_LBA_LO, lba & 0xFF);
	outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
	outb(ATA_LBA_HI, (lba >> 16) & 0xFF);
	outb(ATA_CMD, cmd);
	return 0;
}

int ata_read(unsigned int lba, unsigned int count, void *buffer)
{
	unsigned short *buf = (unsigned short *)buffer;

	if (ata_select(lba, count, CMD_READ_SECTORS) != 0)
		return -1;

	for (unsigned int s = 0; s < count; s++) {
		if (ata_poll() != 0)
			return -1;
		for (int i = 0; i < 256; i++)           /* 256 words = 512 bytes */
			buf[i] = inw(ATA_DATA);
		buf += 256;
	}
	return 0;
}

int ata_write(unsigned int lba, unsigned int count, const void *buffer)
{
	const unsigned short *buf = (const unsigned short *)buffer;

	if (ata_select(lba, count, CMD_WRITE_SECTORS) != 0)
		return -1;

	for (unsigned int s = 0; s < count; s++) {
		if (ata_poll() != 0)
			return -1;
		for (int i = 0; i < 256; i++)           /* 256 words = 512 bytes */
			outw(ATA_DATA, buf[i]);
		buf += 256;
	}

	outb(ATA_CMD, CMD_FLUSH_CACHE);                 /* commit to the medium */
	return ata_wait_ready();
}

int ata_identify(char *model_out, unsigned int *sectors_out)
{
	if (ata_select(0, 0, CMD_IDENTIFY) != 0)
		return -1;
	if (ata_poll() != 0)
		return -1;

	unsigned short data[256];
	for (int i = 0; i < 256; i++)
		data[i] = inw(ATA_DATA);

	/* Words 27-46 hold the model string, but each word stores its two
	 * characters byte-swapped relative to normal string order. */
	for (int w = 0; w < 20; w++) {
		unsigned short word = data[27 + w];
		model_out[w * 2]     = (char)(word >> 8);
		model_out[w * 2 + 1] = (char)(word & 0xFF);
	}
	model_out[40] = '\0';
	for (int i = 39; i >= 0 && model_out[i] == ' '; i--)
		model_out[i] = '\0';

	*sectors_out = ((unsigned int)data[61] << 16) | data[60];
	return 0;
}
