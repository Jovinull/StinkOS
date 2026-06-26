/* MBR partition table read/write. Sector 0 is read into a 512-byte buffer,
 * its 4 partition slots are decoded/encoded little-endian, and the 0x55AA
 * signature is enforced on read + set on write. The bootstrap region
 * (bytes 0..445) is preserved on every write so an installer can fill it
 * with the boot loader independently. */
#include "mbr.h"
#include "ata.h"

#define MBR_ENTRY_OFFSET   446
#define MBR_ENTRY_SIZE     16
#define MBR_SIG_OFFSET     510

static unsigned int read_le32(const unsigned char *p)
{
	return  (unsigned int)p[0]        |
	       ((unsigned int)p[1] <<  8) |
	       ((unsigned int)p[2] << 16) |
	       ((unsigned int)p[3] << 24);
}

static void write_le32(unsigned char *p, unsigned int v)
{
	p[0] = (unsigned char)( v        & 0xFFu);
	p[1] = (unsigned char)((v >>  8) & 0xFFu);
	p[2] = (unsigned char)((v >> 16) & 0xFFu);
	p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

int mbr_read(int drive, struct mbr_partition out[MBR_PARTS])
{
	unsigned char buf[512];
	if (ata_drive_read(drive, 0, 1, buf) != 0)
		return -1;
	if (buf[MBR_SIG_OFFSET] != 0x55u || buf[MBR_SIG_OFFSET + 1] != 0xAAu)
		return -1;

	for (int i = 0; i < MBR_PARTS; i++) {
		const unsigned char *p = buf + MBR_ENTRY_OFFSET + i * MBR_ENTRY_SIZE;
		out[i].bootable     = p[0];
		out[i].type         = p[4];
		out[i].first_lba    = read_le32(p + 8);
		out[i].sector_count = read_le32(p + 12);
	}
	return 0;
}

int mbr_write(int drive, const struct mbr_partition parts[MBR_PARTS])
{
	unsigned char buf[512];

	/* Read first so we keep whatever bootstrap code is already there. If
	 * the disk is completely blank (or the read fails), start from zero. */
	if (ata_drive_read(drive, 0, 1, buf) != 0) {
		for (int i = 0; i < 512; i++)
			buf[i] = 0;
	}

	for (int i = 0; i < MBR_PARTS; i++) {
		unsigned char *p = buf + MBR_ENTRY_OFFSET + i * MBR_ENTRY_SIZE;
		for (int k = 0; k < MBR_ENTRY_SIZE; k++)
			p[k] = 0;
		p[0] = parts[i].bootable;
		p[4] = parts[i].type;
		write_le32(p +  8, parts[i].first_lba);
		write_le32(p + 12, parts[i].sector_count);
	}

	buf[MBR_SIG_OFFSET]     = 0x55u;
	buf[MBR_SIG_OFFSET + 1] = 0xAAu;

	return ata_drive_write(drive, 0, 1, buf);
}
