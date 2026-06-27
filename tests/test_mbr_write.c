/* Host-side test for the MBR partition-table encoding done by
 * kernel/fs/mbr.c (mbr_write). The kernel reads sector 0, overwrites
 * bytes 446..509 with the 4 partition entries (16 bytes each), sets
 * the 0x55AA signature at 510..511, and writes the sector back.
 *
 * Wire layout per entry (RFC-stable, used by every PC since 1983):
 *   off 0  : bootable flag (0x80 = active, 0x00 = inactive)
 *   off 4  : partition type
 *   off 8  : first_lba (little-endian 32-bit)
 *   off 12 : sector_count (little-endian 32-bit)
 *
 * The fields between (1-3, 5-7) are CHS coordinates that modern
 * tooling doesn't trust; mbr_write zeroes them, matching the way
 * Linux's `parted` writes them when it doesn't have a geometry hint.
 *
 * Bootstrap bytes 0..445 of the source sector are preserved -- the
 * installer relies on this so its boot code survives the partition
 * table write.
 */
#include <stdio.h>
#include <string.h>

#define MBR_ENTRY_OFFSET 446
#define MBR_ENTRY_SIZE   16
#define MBR_SIG_OFFSET   510

struct mbr_partition {
	unsigned char bootable;
	unsigned char type;
	unsigned int  first_lba;
	unsigned int  sector_count;
};

static void write_le32(unsigned char *p, unsigned int v)
{
	p[0] = (unsigned char)( v        & 0xFFu);
	p[1] = (unsigned char)((v >>  8) & 0xFFu);
	p[2] = (unsigned char)((v >> 16) & 0xFFu);
	p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

/* Mirrors mbr_write minus the disk I/O. Takes a 512-byte buffer
 * pre-filled with the sector contents to preserve, patches in the
 * partition entries + signature. */
static void mbr_encode(unsigned char *buf, const struct mbr_partition parts[4])
{
	for (int i = 0; i < 4; i++) {
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
}

static int expect_uint(const char *label, unsigned int got, unsigned int want)
{
	if (got == want) { printf("ok   %-50s = 0x%02X\n", label, got); return 0; }
	printf("FAIL %s: got 0x%X, want 0x%X\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;
	unsigned char buf[512];

	/* Pre-fill with sentinel pattern to verify bootstrap preservation. */
	for (int i = 0; i < 446; i++) buf[i] = 0xAA;
	for (int i = 446; i < 512; i++) buf[i] = 0xCC;

	struct mbr_partition parts[4] = {
		{ 0x80, 0x7F, 1,     999999 },   /* active StinkOS slice */
		{ 0,    0,    0,     0      },
		{ 0,    0,    0,     0      },
		{ 0,    0,    0,     0      },
	};
	mbr_encode(buf, parts);

	/* Bootstrap preserved. */
	int ok = 1;
	for (int i = 0; i < 446; i++) if (buf[i] != 0xAA) { ok = 0; break; }
	failures += expect_uint("bootstrap 0..445 preserved (0xAA)", ok, 1);

	/* Signature stamped. */
	failures += expect_uint("sig byte 510 = 0x55", buf[510], 0x55);
	failures += expect_uint("sig byte 511 = 0xAA", buf[511], 0xAA);

	/* Entry 0 fields. */
	failures += expect_uint("entry 0 bootable = 0x80",    buf[446],       0x80);
	failures += expect_uint("entry 0 type = 0x7F",        buf[446 + 4],   0x7F);
	failures += expect_uint("entry 0 first_lba LE byte 0",buf[446 + 8],   0x01);
	failures += expect_uint("entry 0 first_lba LE byte 1",buf[446 + 9],   0x00);
	failures += expect_uint("entry 0 first_lba LE byte 2",buf[446 + 10],  0x00);
	failures += expect_uint("entry 0 first_lba LE byte 3",buf[446 + 11],  0x00);
	/* 999999 = 0x0F423F => LE bytes 3F 42 0F 00 */
	failures += expect_uint("entry 0 count byte 0 = 0x3F",buf[446 + 12],  0x3F);
	failures += expect_uint("entry 0 count byte 1 = 0x42",buf[446 + 13],  0x42);
	failures += expect_uint("entry 0 count byte 2 = 0x0F",buf[446 + 14],  0x0F);
	failures += expect_uint("entry 0 count byte 3 = 0x00",buf[446 + 15],  0x00);

	/* Entries 1..3 fully zeroed (no stale 0xCC). */
	int z = 1;
	for (int i = 446 + 16; i < 446 + 64; i++) if (buf[i] != 0x00) { z = 0; break; }
	failures += expect_uint("entries 1..3 fully zeroed", z, 1);

	/* CHS fields of entry 0 (offsets 1..3 and 5..7) zeroed. */
	failures += expect_uint("entry 0 CHS byte 1 = 0", buf[446 + 1], 0);
	failures += expect_uint("entry 0 CHS byte 2 = 0", buf[446 + 2], 0);
	failures += expect_uint("entry 0 CHS byte 3 = 0", buf[446 + 3], 0);
	failures += expect_uint("entry 0 CHS byte 5 = 0", buf[446 + 5], 0);
	failures += expect_uint("entry 0 CHS byte 6 = 0", buf[446 + 6], 0);
	failures += expect_uint("entry 0 CHS byte 7 = 0", buf[446 + 7], 0);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
