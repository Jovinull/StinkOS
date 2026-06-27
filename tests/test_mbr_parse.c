/* Host-side test for the MBR partition table parser in kernel/fs/mbr.c.
 * The relevant logic is purely byte-decoding: little-endian u32 read of
 * first_lba and sector_count, signature check at offset 510. We replicate
 * the parse path and exercise it against handcrafted 512-byte sectors.
 */
#include <stdio.h>
#include <string.h>

#define MBR_ENTRY_OFFSET 446
#define MBR_ENTRY_SIZE   16
#define MBR_SIG_OFFSET   510
#define MBR_PARTS        4

struct partition {
	unsigned char  bootable;
	unsigned char  type;
	unsigned int   first_lba;
	unsigned int   sector_count;
};

static unsigned int read_le32(const unsigned char *p)
{
	return  (unsigned int)p[0]        |
	       ((unsigned int)p[1] <<  8) |
	       ((unsigned int)p[2] << 16) |
	       ((unsigned int)p[3] << 24);
}

/* Returns 0 on success (signature present), -1 on missing signature. */
static int mbr_parse(const unsigned char *sector, struct partition out[MBR_PARTS])
{
	if (sector[MBR_SIG_OFFSET] != 0x55u || sector[MBR_SIG_OFFSET + 1] != 0xAAu)
		return -1;
	for (int i = 0; i < MBR_PARTS; i++) {
		const unsigned char *p = sector + MBR_ENTRY_OFFSET + i * MBR_ENTRY_SIZE;
		out[i].bootable     = p[0];
		out[i].type         = p[4];
		out[i].first_lba    = read_le32(p + 8);
		out[i].sector_count = read_le32(p + 12);
	}
	return 0;
}

static int expect_int(const char *label, int got, int want)
{
	if (got == want) { printf("ok   %-40s = %d\n", label, got); return 0; }
	printf("FAIL %s: got %d, want %d\n", label, got, want);
	return 1;
}

static int expect_u32(const char *label, unsigned int got, unsigned int want)
{
	if (got == want) { printf("ok   %-40s = 0x%08x\n", label, got); return 0; }
	printf("FAIL %s: got 0x%08x, want 0x%08x\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;
	unsigned char sector[512];

	/* Empty sector (no signature) -> parse fails. */
	memset(sector, 0, sizeof(sector));
	struct partition p[4];
	failures += expect_int("missing signature rejected", mbr_parse(sector, p), -1);

	/* Add signature; partitions are still all zero (valid but empty). */
	sector[MBR_SIG_OFFSET]     = 0x55;
	sector[MBR_SIG_OFFSET + 1] = 0xAA;
	failures += expect_int("signature present accepted", mbr_parse(sector, p), 0);
	failures += expect_u32("empty entry first_lba",       p[0].first_lba, 0);
	failures += expect_u32("empty entry sector_count",    p[0].sector_count, 0);

	/* Stuff entry 1 with a bootable StinkOS partition starting at LBA 1,
	 * count 200128 (covers the 100 MiB StinkFS data region). */
	unsigned char *e1 = sector + MBR_ENTRY_OFFSET;
	e1[0]  = 0x80;           /* bootable */
	e1[4]  = 0x53;           /* type StinkOS */
	e1[8]  = 0x01; e1[9]  = 0x00; e1[10] = 0x00; e1[11] = 0x00;   /* LBA 1 */
	e1[12] = 0x80; e1[13] = 0x10; e1[14] = 0x03; e1[15] = 0x00;   /* count 200320 */
	failures += expect_int("decoded sig still OK", mbr_parse(sector, p), 0);
	failures += expect_int("entry 1 bootable",     p[0].bootable, 0x80);
	failures += expect_int("entry 1 type",         p[0].type,     0x53);
	failures += expect_u32("entry 1 first_lba",    p[0].first_lba, 1);
	failures += expect_u32("entry 1 sector_count", p[0].sector_count, 0x031080);

	/* Stuff entry 4 with a 32-bit wraparound value to check the LE32
	 * decoder handles the high byte correctly. */
	unsigned char *e4 = sector + MBR_ENTRY_OFFSET + 3 * MBR_ENTRY_SIZE;
	e4[8]  = 0x00; e4[9]  = 0x00; e4[10] = 0x00; e4[11] = 0x80;   /* LBA 2 GiB */
	e4[12] = 0xFF; e4[13] = 0xFF; e4[14] = 0xFF; e4[15] = 0xFF;   /* max u32 */
	failures += expect_int("decoded sig OK after entry 4", mbr_parse(sector, p), 0);
	failures += expect_u32("entry 4 first_lba",   p[3].first_lba,    0x80000000u);
	failures += expect_u32("entry 4 sector_count",p[3].sector_count, 0xFFFFFFFFu);

	/* Wrong signature byte 1 -> reject. */
	sector[MBR_SIG_OFFSET] = 0x54;
	failures += expect_int("wrong sig byte 1 rejected", mbr_parse(sector, p), -1);
	sector[MBR_SIG_OFFSET] = 0x55;
	sector[MBR_SIG_OFFSET + 1] = 0xAB;
	failures += expect_int("wrong sig byte 2 rejected", mbr_parse(sector, p), -1);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
