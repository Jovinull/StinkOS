/* Master Boot Record helpers: read/write the 512-byte sector-0 partition
 * table that classical x86 PCs use. The kernel uses this so the installer
 * (Modelo B) can lay out a fresh target disk with a single bootable
 * StinkOS partition, and so the boot path can locate the OS root on a
 * partitioned disk later.
 *
 * MBR layout (bytes from start of sector):
 *    0..445  bootstrap code (we leave it untouched on writes)
 *  446..461  partition entry 1
 *  462..477  partition entry 2
 *  478..493  partition entry 3
 *  494..509  partition entry 4
 *  510..511  signature: 0x55 0xAA
 *
 * Partition entry (16 bytes):
 *    0       boot flag (0x80 = bootable, 0x00 = not)
 *    1..3    legacy CHS first sector (ignored; we always use LBA)
 *    4       type byte (0x83 = Linux, 0x53 = StinkOS for now)
 *    5..7    legacy CHS last sector (ignored)
 *    8..11   LBA of first sector (little-endian u32)
 *   12..15   sector count (little-endian u32)
 */
#ifndef MBR_H
#define MBR_H

#define MBR_PARTS         4
#define MBR_TYPE_STINKOS  0x53     /* arbitrary picked code, not in use elsewhere */
#define MBR_TYPE_LINUX    0x83
#define MBR_TYPE_EMPTY    0x00
#define MBR_BOOTABLE      0x80

struct mbr_partition {
	unsigned char  bootable;
	unsigned char  type;
	unsigned int   first_lba;
	unsigned int   sector_count;
};

/* Read the partition table from drive's sector 0 into 'out' (4 entries).
 * Returns 0 on success, -1 on read failure or missing 0x55AA signature. */
int mbr_read(int drive, struct mbr_partition out[MBR_PARTS]);

/* Write a partition table to drive's sector 0. Preserves the existing
 * bootstrap code (bytes 0..445); rewrites only the 4 partition entries +
 * signature. Returns 0 on success, -1 on I/O failure. */
int mbr_write(int drive, const struct mbr_partition parts[MBR_PARTS]);

#endif
