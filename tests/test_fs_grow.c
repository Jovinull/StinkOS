/* Host-side test for the fs_grow allocator branch logic in
 * kernel/fs/fs.c. fs_grow has three paths the directory bookkeeping
 * needs to land on correctly:
 *
 *   1. new_sect <= old_sect: file already fits, no-op.
 *   2. file sits at the high-water mark (start + old_sect == next_free):
 *      extend in place by bumping next_free, no relocation.
 *   3. otherwise: relocate to next_free (copy the old sectors over
 *      via ATA), advance next_free, leave the old region orphaned.
 *
 * The sector arithmetic and which branch fires determine whether
 * the disk space stays compact or fragments fast -- both silent in
 * QEMU. We exercise the branch decisions and the next_free bumps
 * without touching the ATA layer.
 */
#include <stdio.h>

#define FS_DATA_LBA   130u
#define FS_DATA_END   16384u
#define FS_MAX_FILES  40

struct fs_file {
	char         name[16];
	unsigned int start;
	unsigned int size;
};

struct fs_dir {
	unsigned int   count;
	unsigned int   next_free;
	struct fs_file files[FS_MAX_FILES];
};

static struct fs_dir dir;

static unsigned int need_sectors(unsigned int size)
{
	return (size + 511) / 512;
}

enum grow_path { GROW_NOOP, GROW_INPLACE, GROW_RELOCATE, GROW_FAIL };

/* Mirror of fs_grow that returns which branch fired without doing
 * any disk I/O. */
static int fs_grow_sim(int i, unsigned int new_size)
{
	unsigned int start    = dir.files[i].start;
	unsigned int old_sect = need_sectors(dir.files[i].size);
	unsigned int new_sect = need_sectors(new_size);

	if (new_sect <= old_sect) return GROW_NOOP;

	if (start + old_sect == dir.next_free) {
		if (start + new_sect > FS_DATA_END) return GROW_FAIL;
		dir.next_free = start + new_sect;
		return GROW_INPLACE;
	}

	if (dir.next_free + new_sect > FS_DATA_END) return GROW_FAIL;
	dir.next_free += new_sect;
	dir.files[i].start = dir.next_free - new_sect;
	return GROW_RELOCATE;
}

static int file_create(const char *name, unsigned int size)
{
	int i = (int)dir.count++;
	for (int k = 0; k < 16; k++) dir.files[i].name[k] = name[k];
	dir.files[i].start = dir.next_free;
	dir.files[i].size  = size;
	dir.next_free     += need_sectors(size);
	return i;
}

static void dir_reset(void)
{
	dir.count     = 0;
	dir.next_free = FS_DATA_LBA;
}

static int expect_int(const char *label, int got, int want)
{
	if (got == want) { printf("ok   %-55s = %d\n", label, got); return 0; }
	printf("FAIL %s: got %d, want %d\n", label, got, want);
	return 1;
}

static int expect_u(const char *label, unsigned int got, unsigned int want)
{
	if (got == want) { printf("ok   %-55s = %u\n", label, got); return 0; }
	printf("FAIL %s: got %u, want %u\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;
	int a, b;

	/* --- noop: new size fits in the same sector count ------------- */
	dir_reset();
	a = file_create("a", 600);            /* 2 sectors */
	failures += expect_int("grow within same sectors -> NOOP",
	                       fs_grow_sim(a, 700), GROW_NOOP);
	failures += expect_u("next_free unchanged",
	                     dir.next_free, FS_DATA_LBA + 2u);

	/* --- in-place: file at the high-water mark, bump next_free --- */
	dir_reset();
	a = file_create("a", 100);            /* 1 sector */
	failures += expect_int("grow to 600 from end-of-pool -> INPLACE",
	                       fs_grow_sim(a, 600), GROW_INPLACE);
	failures += expect_u("INPLACE bumps next_free by 1",
	                     dir.next_free, FS_DATA_LBA + 2u);
	failures += expect_u("INPLACE file start unchanged",
	                     dir.files[a].start, FS_DATA_LBA);

	/* --- relocate: another file sits after us, can't extend in place */
	dir_reset();
	a = file_create("a", 100);            /* sector LBA 130 */
	b = file_create("b", 100);            /* sector LBA 131 */
	(void)b;
	failures += expect_int("grow A past B -> RELOCATE",
	                       fs_grow_sim(a, 1024), GROW_RELOCATE);
	failures += expect_u("RELOCATE moves A to former next_free",
	                     dir.files[a].start, FS_DATA_LBA + 2u);
	failures += expect_u("RELOCATE advances next_free by new sectors",
	                     dir.next_free, FS_DATA_LBA + 2u + 2u);
	/* The orphaned [130, 132) is intentional (no compactor). */

	/* --- fail: in-place extension hits FS_DATA_END --------------- */
	dir_reset();
	a = file_create("a", 100);
	dir.next_free = FS_DATA_END - 1;       /* only one free sector left */
	dir.files[a].start = FS_DATA_END - 2;
	dir.files[a].size  = 100;              /* still 1 sector */
	failures += expect_int("INPLACE blocked by FS_DATA_END -> FAIL",
	                       fs_grow_sim(a, 2048), GROW_FAIL);

	/* --- fail: relocate cannot fit ------------------------------- */
	dir_reset();
	a = file_create("a", 100);
	b = file_create("b", 100);
	(void)b;
	dir.next_free = FS_DATA_END;           /* nothing free anymore */
	failures += expect_int("RELOCATE blocked by FS_DATA_END -> FAIL",
	                       fs_grow_sim(a, 1024), GROW_FAIL);

	/* --- need_sectors edge cases -------------------------------- */
	failures += expect_u("need_sectors(0) = 0",       need_sectors(0),     0);
	failures += expect_u("need_sectors(1) = 1",       need_sectors(1),     1);
	failures += expect_u("need_sectors(512) = 1",     need_sectors(512),   1);
	failures += expect_u("need_sectors(513) = 2",     need_sectors(513),   2);
	failures += expect_u("need_sectors(1024) = 2",    need_sectors(1024),  2);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
