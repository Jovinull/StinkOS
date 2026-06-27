/* Host-side test for the StinkFS directory helpers in kernel/fs/fs.c:
 *   - the on-disk magic value endianness ('S','T','N','K' LE = 0x4B4E5453)
 *   - name_eq      (case-sensitive 16-byte compare)
 *   - name_ci_eq   (case-insensitive 16-byte compare; ASCII only)
 *   - fs_find      (linear scan of dir->files for an exact name match)
 *   - need_sectors (round-up byte count to 512-byte sector count)
 *   - the lazy-init contract: an uninitialised directory sector (magic
 *     mismatch) must be wiped to empty rather than trusted.
 *
 * The kernel logic is replicated here; any change to fs.c's helpers
 * should land in this file too so the test fails loudly.
 */
#include <stdio.h>
#include <string.h>

#define STINKFS_MAGIC 0x4B4E5453u
#define FS_MAX_FILES  40
#define FS_DATA_LBA   130u

struct fs_file {
	char         name[16];
	unsigned int start;
	unsigned int size;
};

struct fs_dir {
	unsigned int   magic;
	unsigned int   count;
	unsigned int   next_free;
	struct fs_file files[FS_MAX_FILES];
};

/* --- replica of fs.c helpers ------------------------------------------- */

static int name_eq(const char *a, const char *b)
{
	for (int i = 0; i < 16; i++)
		if (a[i] != b[i]) return 0;
	return 1;
}

static int name_ci_eq(const char *a, const char *b)
{
	for (int i = 0; i < 16; i++) {
		char ca = a[i], cb = b[i];
		if (ca >= 'a' && ca <= 'z') ca -= 32;
		if (cb >= 'a' && cb <= 'z') cb -= 32;
		if (ca != cb) return 0;
	}
	return 1;
}

static int fs_find(const struct fs_dir *d, const char *name)
{
	for (unsigned int i = 0; i < d->count; i++)
		if (name_eq(d->files[i].name, name))
			return (int)i;
	return -1;
}

static unsigned int need_sectors(unsigned int size)
{
	return (size + 511) / 512;
}

static void fs_dir_lazy_init(struct fs_dir *d)
{
	if (d->magic != STINKFS_MAGIC) {
		d->magic     = STINKFS_MAGIC;
		d->count     = 0;
		d->next_free = FS_DATA_LBA;
	}
}

/* --- helpers ----------------------------------------------------------- */

static void set_name(char dst[16], const char *src)
{
	memset(dst, 0, 16);
	for (int i = 0; i < 16 && src[i]; i++) dst[i] = src[i];
}

static int expect_int(const char *label, int got, int want)
{
	if (got == want) { printf("ok   %-45s = %d\n", label, got); return 0; }
	printf("FAIL %s: got %d, want %d\n", label, got, want);
	return 1;
}

static int expect_uint(const char *label, unsigned int got, unsigned int want)
{
	if (got == want) { printf("ok   %-45s = 0x%08x\n", label, got); return 0; }
	printf("FAIL %s: got 0x%08x, want 0x%08x\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;
	struct fs_dir d;

	/* --- magic byte order ---------------------------------------------- */
	{
		unsigned char *mp = (unsigned char *)&(unsigned int){STINKFS_MAGIC};
		failures += expect_uint("STINKFS_MAGIC value", STINKFS_MAGIC, 0x4B4E5453u);
		/* On the wire (little-endian), bytes 0..3 spell S T N K. */
		unsigned int v = STINKFS_MAGIC;
		(void)mp;
		failures += expect_int("magic byte 0 = 'S'", (int)(v        & 0xFF), 'S');
		failures += expect_int("magic byte 1 = 'T'", (int)((v >>  8) & 0xFF), 'T');
		failures += expect_int("magic byte 2 = 'N'", (int)((v >> 16) & 0xFF), 'N');
		failures += expect_int("magic byte 3 = 'K'", (int)((v >> 24) & 0xFF), 'K');
	}

	/* --- need_sectors boundaries -------------------------------------- */
	failures += expect_int("need_sectors(0)",       (int)need_sectors(0),       0);
	failures += expect_int("need_sectors(1)",       (int)need_sectors(1),       1);
	failures += expect_int("need_sectors(511)",     (int)need_sectors(511),     1);
	failures += expect_int("need_sectors(512)",     (int)need_sectors(512),     1);
	failures += expect_int("need_sectors(513)",     (int)need_sectors(513),     2);
	failures += expect_int("need_sectors(1024)",    (int)need_sectors(1024),    2);
	failures += expect_int("need_sectors(100000)",  (int)need_sectors(100000),  196);

	/* --- name_eq case-sensitive --------------------------------------- */
	{
		char a[16], b[16];
		set_name(a, "DOOM.WAD");
		set_name(b, "DOOM.WAD");
		failures += expect_int("name_eq identical",       name_eq(a, b), 1);
		set_name(b, "doom.wad");
		failures += expect_int("name_eq lowercase differ", name_eq(a, b), 0);
		set_name(b, "DOOM.WAd");
		failures += expect_int("name_eq one byte differ", name_eq(a, b), 0);
	}

	/* --- name_ci_eq case-insensitive --------------------------------- */
	{
		char a[16], b[16];
		set_name(a, "DOOM.WAD");
		set_name(b, "doom.wad");
		failures += expect_int("name_ci_eq mixed case",   name_ci_eq(a, b), 1);
		set_name(b, "DOOM.WAd");
		failures += expect_int("name_ci_eq mixed letter", name_ci_eq(a, b), 1);
		set_name(b, "OTHER.WAD");
		failures += expect_int("name_ci_eq differs",      name_ci_eq(a, b), 0);
	}

	/* --- fs_find ------------------------------------------------------- */
	/* name_eq reads all 16 bytes -- inputs must be padded to 16 with NUL,
	 * matching the on-disk layout. Passing a bare C string literal would
	 * UB-read past the literal's null terminator. */
	memset(&d, 0, sizeof(d));
	d.magic = STINKFS_MAGIC;
	d.count = 3;
	set_name(d.files[0].name, "BOOT.CFG");
	set_name(d.files[1].name, "DOOM.WAD");
	set_name(d.files[2].name, "STINK.CONF");
	char q[16];
	set_name(q, "BOOT.CFG");    failures += expect_int("fs_find existing first",  fs_find(&d, q),  0);
	set_name(q, "DOOM.WAD");    failures += expect_int("fs_find existing middle", fs_find(&d, q),  1);
	set_name(q, "STINK.CONF");  failures += expect_int("fs_find existing last",   fs_find(&d, q),  2);
	set_name(q, "NOPE.TXT");    failures += expect_int("fs_find missing",         fs_find(&d, q), -1);
	/* fs_find is case-sensitive (matches name_eq). */
	set_name(q, "doom.wad");    failures += expect_int("fs_find wrong case",      fs_find(&d, q), -1);

	/* --- lazy init: blank disk gets a fresh empty directory ----------- */
	memset(&d, 0xFF, sizeof(d));   /* simulate uninitialised sector */
	fs_dir_lazy_init(&d);
	failures += expect_uint("lazy init: magic set",      d.magic,     STINKFS_MAGIC);
	failures += expect_uint("lazy init: count cleared",  d.count,     0u);
	failures += expect_uint("lazy init: next_free seed", d.next_free, FS_DATA_LBA);

	/* Already-initialised directory is left alone. */
	memset(&d, 0, sizeof(d));
	d.magic     = STINKFS_MAGIC;
	d.count     = 7;
	d.next_free = 9999;
	fs_dir_lazy_init(&d);
	failures += expect_uint("already-init: count kept",      d.count,     7u);
	failures += expect_uint("already-init: next_free kept",  d.next_free, 9999u);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
