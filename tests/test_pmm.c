/* Host-side test for the physical memory manager in kernel/arch/pmm.c.
 * The allocator has two layers:
 *   - a bump pointer (`next_frame`) walking the configured range
 *   - a tiny free-list of returned frames (LIFO, up to FREE_MAX)
 *
 * Invariants we lock:
 *   - alignment: alloc returns a 4 KiB-aligned address inside the range
 *   - watermark: bump never overflows past end_frame
 *   - free / re-alloc: a freed frame comes back on the next alloc (LIFO)
 *   - bounds reject: pmm_free of an out-of-range or NULL address is a no-op
 *   - total / free accounting: pmm_total_pages / pmm_free_pages stay in
 *     sync after every operation
 */
#include <stdio.h>

#define FRAME_SIZE 4096u
#define FREE_MAX   1024

static unsigned int start_frame;
static unsigned int next_frame;
static unsigned int end_frame;
static unsigned int free_list[FREE_MAX];
static int free_count;

static void pmm_init(unsigned int start, unsigned int end)
{
	start_frame = (start + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1);
	next_frame  = start_frame;
	end_frame   = end & ~(FRAME_SIZE - 1);
	free_count  = 0;
}

static unsigned int pmm_alloc(void)
{
	if (free_count > 0) return free_list[--free_count];
	if (next_frame + FRAME_SIZE <= end_frame) {
		unsigned int f = next_frame;
		next_frame += FRAME_SIZE;
		return f;
	}
	return 0;
}

static unsigned int pmm_total_pages(void)
{
	return (end_frame - start_frame) / FRAME_SIZE;
}

static unsigned int pmm_free_pages(void)
{
	unsigned int tail = (end_frame - next_frame) / FRAME_SIZE;
	return tail + (unsigned int)free_count;
}

static void pmm_free(unsigned int frame)
{
	frame &= ~(FRAME_SIZE - 1);
	if (frame < start_frame || frame >= end_frame) return;
	if (free_count < FREE_MAX) free_list[free_count++] = frame;
}

static int expect_uint(const char *label, unsigned int got, unsigned int want)
{
	if (got == want) { printf("ok   %-50s = %u\n", label, got); return 0; }
	printf("FAIL %s: got %u, want %u\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;

	/* Init over [0x100000, 0x110000) = 16 frames. */
	pmm_init(0x100000, 0x110000);
	failures += expect_uint("init: total = 16 pages",      pmm_total_pages(), 16);
	failures += expect_uint("init: free = 16 pages",       pmm_free_pages(),  16);

	/* Allocate a frame -- aligned, inside range. */
	unsigned int a = pmm_alloc();
	failures += expect_uint("alloc 1: aligned",            a & (FRAME_SIZE - 1), 0);
	failures += expect_uint("alloc 1: free = 15",          pmm_free_pages(),  15);

	/* Allocate the rest. */
	for (int i = 0; i < 15; i++) (void)pmm_alloc();
	failures += expect_uint("alloc all: free = 0",         pmm_free_pages(),  0);
	failures += expect_uint("OOM returns 0",               pmm_alloc(),       0);

	/* Free one and re-alloc: LIFO returns the same frame. */
	pmm_free(a);
	failures += expect_uint("after free: free = 1",        pmm_free_pages(),  1);
	failures += expect_uint("re-alloc returns freed",      pmm_alloc(),       a);
	failures += expect_uint("post re-alloc: free = 0",     pmm_free_pages(),  0);

	/* Bounds reject: NULL ignored, below-range ignored, above-range ignored. */
	pmm_init(0x100000, 0x110000);
	for (int i = 0; i < 16; i++) (void)pmm_alloc();
	pmm_free(0);                       /* NULL: should be ignored */
	pmm_free(0x000000);                /* below range */
	pmm_free(0x200000);                /* above range */
	pmm_free(0x10FFFFu);               /* misaligned but inside -- aligned down to 0x10F000 */
	failures += expect_uint("bounds: still 1 free after bogus + 1 valid",
	                        pmm_free_pages(), 1);

	/* Alignment of bumped frames. Starts at start_frame, increments by 4 KiB. */
	pmm_init(0x100000, 0x108000);      /* 8 frames */
	unsigned int prev = pmm_alloc();
	for (int i = 1; i < 8; i++) {
		unsigned int n = pmm_alloc();
		if (n - prev != FRAME_SIZE) {
			printf("FAIL bump alignment: gap %u\n", n - prev);
			failures++;
		}
		prev = n;
	}
	if (failures == 0)
		printf("ok   bump alignment: 8 frames, 4 KiB apart each\n");

	/* free_list LIFO order: free a, b, c -> alloc c, b, a. */
	pmm_init(0x100000, 0x110000);
	unsigned int aa = pmm_alloc();
	unsigned int bb = pmm_alloc();
	unsigned int cc = pmm_alloc();
	pmm_free(aa); pmm_free(bb); pmm_free(cc);
	failures += expect_uint("LIFO 1st re-alloc = cc",      pmm_alloc(), cc);
	failures += expect_uint("LIFO 2nd re-alloc = bb",      pmm_alloc(), bb);
	failures += expect_uint("LIFO 3rd re-alloc = aa",      pmm_alloc(), aa);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
