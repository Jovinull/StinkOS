/* ATA PIO disk driver, multi-drive aware (LBA28, polling, no IRQ).
 *
 * Drive index layout:
 *   0 = primary master   (I/O base 0x1F0, drive byte 0xE0)
 *   1 = primary slave    (I/O base 0x1F0, drive byte 0xF0)
 *   2 = secondary master (I/O base 0x170, drive byte 0xE0)
 *   3 = secondary slave  (I/O base 0x170, drive byte 0xF0)
 *
 * The existing ata_read / ata_write / ata_identify functions stay as thin
 * wrappers around drive 0 so every existing caller (fs.c, vfs.c, elf.c,
 * menu.c) keeps compiling unchanged. New code (installer, future per-drive
 * features) uses the drive-aware variants.
 */
#include "ata.h"
#include "io.h"

#define ATA_PRIMARY_BASE   0x1F0
#define ATA_SECONDARY_BASE 0x170

#define ATA_REG_DATA       0x00
#define ATA_REG_SECCOUNT   0x02
#define ATA_REG_LBA_LO     0x03
#define ATA_REG_LBA_MID    0x04
#define ATA_REG_LBA_HI     0x05
#define ATA_REG_DRIVE      0x06
#define ATA_REG_CMD        0x07     /* write: command  / read: status */

#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_BSY 0x80

#define CMD_READ_SECTORS  0x20
#define CMD_WRITE_SECTORS 0x30
#define CMD_FLUSH_CACHE   0xE7
#define CMD_IDENTIFY      0xEC

#define ATA_TIMEOUT_SPINS 1000000

/* Resolve a drive index into the (base port, drive-select byte) pair. */
static unsigned short ata_base_for(int drive)
{
	return (drive < 2) ? ATA_PRIMARY_BASE : ATA_SECONDARY_BASE;
}

static unsigned char ata_drive_byte(int drive)
{
	return (drive & 1) ? 0xF0u : 0xE0u;
}

static int ata_wait_ready_base(unsigned short base)
{
	unsigned int spins = 0;
	while ((inb(base + ATA_REG_CMD) & ST_BSY) && spins < ATA_TIMEOUT_SPINS)
		spins++;
	return (spins < ATA_TIMEOUT_SPINS) ? 0 : -1;
}

static int ata_poll_base(unsigned short base)
{
	if (ata_wait_ready_base(base) != 0)
		return -1;

	unsigned int spins = 0;
	for (;;) {
		unsigned char status = inb(base + ATA_REG_CMD);
		if (status & ST_ERR)
			return -1;
		if (status & ST_DRQ)
			return 0;
		if (++spins >= ATA_TIMEOUT_SPINS)
			return -1;
	}
}

static int ata_select_drive(int drive, unsigned int lba,
                            unsigned int count, unsigned char cmd)
{
	unsigned short base   = ata_base_for(drive);
	unsigned char  dbyte  = ata_drive_byte(drive);

	if (ata_wait_ready_base(base) != 0)
		return -1;

	outb(base + ATA_REG_DRIVE,    dbyte | ((lba >> 24) & 0x0Fu));
	outb(base + ATA_REG_SECCOUNT, (unsigned char)count);
	outb(base + ATA_REG_LBA_LO,   lba & 0xFFu);
	outb(base + ATA_REG_LBA_MID, (lba >> 8) & 0xFFu);
	outb(base + ATA_REG_LBA_HI,  (lba >> 16) & 0xFFu);
	outb(base + ATA_REG_CMD,     cmd);
	return 0;
}

int ata_drive_read(int drive, unsigned int lba, unsigned int count, void *buffer)
{
	if (drive < 0 || drive > 3)
		return -1;
	unsigned short  base = ata_base_for(drive);
	unsigned short *buf  = (unsigned short *)buffer;

	if (ata_select_drive(drive, lba, count, CMD_READ_SECTORS) != 0)
		return -1;

	for (unsigned int s = 0; s < count; s++) {
		if (ata_poll_base(base) != 0)
			return -1;
		for (int i = 0; i < 256; i++)
			buf[i] = inw(base + ATA_REG_DATA);
		buf += 256;
	}
	return 0;
}

int ata_drive_write(int drive, unsigned int lba, unsigned int count, const void *buffer)
{
	if (drive < 0 || drive > 3)
		return -1;
	unsigned short        base = ata_base_for(drive);
	const unsigned short *buf  = (const unsigned short *)buffer;

	if (ata_select_drive(drive, lba, count, CMD_WRITE_SECTORS) != 0)
		return -1;

	for (unsigned int s = 0; s < count; s++) {
		if (ata_poll_base(base) != 0)
			return -1;
		for (int i = 0; i < 256; i++)
			outw(base + ATA_REG_DATA, buf[i]);
		buf += 256;
	}

	outb(base + ATA_REG_CMD, CMD_FLUSH_CACHE);
	return ata_wait_ready_base(base);
}

int ata_drive_identify(int drive, char *model_out, unsigned int *sectors_out)
{
	if (drive < 0 || drive > 3)
		return -1;
	unsigned short base = ata_base_for(drive);

	if (ata_select_drive(drive, 0, 0, CMD_IDENTIFY) != 0)
		return -1;
	if (ata_poll_base(base) != 0)
		return -1;

	unsigned short data[256];
	for (int i = 0; i < 256; i++)
		data[i] = inw(base + ATA_REG_DATA);

	/* Words 27-46 hold the model string, each word storing two characters
	 * byte-swapped relative to normal string order. */
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

/* ---- backward-compat wrappers: drive 0 = primary master ---- */

int ata_read(unsigned int lba, unsigned int count, void *buffer)
{
	return ata_drive_read(0, lba, count, buffer);
}

int ata_write(unsigned int lba, unsigned int count, const void *buffer)
{
	return ata_drive_write(0, lba, count, buffer);
}

int ata_identify(char *model_out, unsigned int *sectors_out)
{
	return ata_drive_identify(0, model_out, sectors_out);
}
