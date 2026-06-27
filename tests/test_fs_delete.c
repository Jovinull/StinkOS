/* Host-side test for fs_file_delete compaction in kernel/fs/fs.c.
 * When a file is deleted, the kernel:
 *   1. shifts every later sector down by `freed` sector count
 *      (so the free pool stays contiguous)
 *   2. updates the `start` field of every later file by -`freed`
 *   3. removes the entry from dir->files by shifting tail entries up
 *   4. decrements dir->count and dir->next_free
 *
 * The hairy invariant is the address-rewrite predicate: it uses
 * `dir->files[j].start > hole` (strict, not >=), because the file
 * being deleted is the only one at exactly `hole`. A regression to
 * `>=` would silently underflow its own (deleted) start field by
 * one extra freed-size during the compaction.
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

static int fs_find(const char *name)
{
	for (unsigned int i = 0; i < dir.count; i++) {
		int eq = 1;
		for (int k = 0; k < 16; k++)
			if (dir.files[i].name[k] != name[k]) { eq = 0; break; }
		if (eq) return (int)i;
	}
	return -1;
}

/* Mirror of fs_file_delete without disk I/O. Returns 0 on ok, -1 on miss. */
static int fs_file_delete_sim(const char *name)
{
	int i = fs_find(name);
	if (i < 0) return -1;

	unsigned int freed = need_sectors(dir.files[i].size);
	unsigned int hole  = dir.files[i].start;

	/* (no sector copy in the sim -- the bookkeeping is what we test) */

	for (unsigned int j = 0; j < dir.count; j++)
		if (dir.files[j].start > hole)
			dir.files[j].start -= freed;

	for (unsigned int j = (unsigned int)i; j + 1 < dir.count; j++)
		dir.files[j] = dir.files[j + 1];
	dir.count--;
	dir.next_free -= freed;
	return 0;
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
	for (int i = 0; i < FS_MAX_FILES; i++) {
		for (int k = 0; k < 16; k++) dir.files[i].name[k] = 0;
		dir.files[i].start = dir.files[i].size = 0;
	}
}

static int expect_u(const char *label, unsigned int got, unsigned int want)
{
	if (got == want) { printf("ok   %-55s = %u\n", label, got); return 0; }
	printf("FAIL %s: got %u, want %u\n", label, got, want);
	return 1;
}

static int expect_int(const char *label, int got, int want)
{
	if (got == want) { printf("ok   %-55s = %d\n", label, got); return 0; }
	printf("FAIL %s: got %d, want %d\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;

	/* --- delete missing -> -1 ----------------------------------- */
	dir_reset();
	failures += expect_int("delete missing -> -1",
	                       fs_file_delete_sim("nope            "), -1);

	/* --- delete the only file: count=0, next_free reset --------- */
	dir_reset();
	file_create("only            ", 1024);
	failures += expect_int("delete only -> 0",
	                       fs_file_delete_sim("only            "), 0);
	failures += expect_u("count after delete only", dir.count, 0);
	failures += expect_u("next_free after delete only",
	                     dir.next_free, FS_DATA_LBA);

	/* --- delete head, others shifted down ---------------------- */
	dir_reset();
	file_create("aaaaaaaaaaaaaaaa", 1024);   /* 2 sectors at 130 */
	file_create("bbbbbbbbbbbbbbbb", 512);    /* 1 sector  at 132 */
	file_create("cccccccccccccccc", 1536);   /* 3 sectors at 133 */
	failures += expect_int("delete head -> 0",
	                       fs_file_delete_sim("aaaaaaaaaaaaaaaa"), 0);
	failures += expect_u("count after head delete", dir.count, 2);
	/* B used to live at 132, now at 132-2=130. C used to live at 133, now 131. */
	failures += expect_u("B shifted to 130", dir.files[0].start, 130);
	failures += expect_u("C shifted to 131", dir.files[1].start, 131);
	failures += expect_u("next_free shrank by freed",
	                     dir.next_free, FS_DATA_LBA + 1u + 3u);

	/* --- delete middle, only tail shifts (head untouched) ------ */
	dir_reset();
	file_create("aaaaaaaaaaaaaaaa", 1024);   /* 130, 2 sectors */
	file_create("bbbbbbbbbbbbbbbb", 1024);   /* 132, 2 sectors */
	file_create("cccccccccccccccc", 1024);   /* 134, 2 sectors */
	failures += expect_int("delete middle -> 0",
	                       fs_file_delete_sim("bbbbbbbbbbbbbbbb"), 0);
	failures += expect_u("A start untouched",  dir.files[0].start, 130);
	failures += expect_u("C shifted to 132",   dir.files[1].start, 132);
	failures += expect_u("count=2 after delete middle", dir.count, 2);

	/* --- delete tail: no shifts, just count--/next_free-- ------ */
	dir_reset();
	file_create("aaaaaaaaaaaaaaaa", 1024);
	file_create("bbbbbbbbbbbbbbbb", 1024);
	file_create("cccccccccccccccc", 1024);
	failures += expect_int("delete tail -> 0",
	                       fs_file_delete_sim("cccccccccccccccc"), 0);
	failures += expect_u("A untouched", dir.files[0].start, 130);
	failures += expect_u("B untouched", dir.files[1].start, 132);
	failures += expect_u("count=2 after delete tail", dir.count, 2);
	failures += expect_u("next_free = end of B",
	                     dir.next_free, 132 + 2);

	/* --- delete the head twice in a row: B becomes the new head */
	dir_reset();
	file_create("aaaaaaaaaaaaaaaa", 1024);
	file_create("bbbbbbbbbbbbbbbb", 512);
	(void)fs_file_delete_sim("aaaaaaaaaaaaaaaa");
	failures += expect_int("re-find a -> -1",
	                       fs_find("aaaaaaaaaaaaaaaa"), -1);
	failures += expect_int("re-find b -> index 0",
	                       fs_find("bbbbbbbbbbbbbbbb"), 0);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
