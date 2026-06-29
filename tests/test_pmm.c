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
 *   - refcount (v0.7 COW): alloc sets refcount=1, ref_inc bumps, free
 *     decrements + only returns to pool at 0
 */
#include <stdio.h>

#define FRAME_SIZE 4096u
#define FREE_MAX   1024
#define MAX_FRAMES 8192

static unsigned int start_frame;
static unsigned int next_frame;
static unsigned int end_frame;
static unsigned int free_list[FREE_MAX];
static int free_count;
static unsigned char refcount[MAX_FRAMES];

static int in_range(unsigned int frame)
{
	return frame >= start_frame && frame < end_frame;
}

static unsigned int frame_idx(unsigned int frame)
{
	return (frame - start_frame) / FRAME_SIZE;
}

static void pmm_init(unsigned int start, unsigned int end)
{
	start_frame = (start + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1);
	next_frame  = start_frame;
	end_frame   = end & ~(FRAME_SIZE - 1);
	free_count  = 0;
	for (unsigned int i = 0; i < MAX_FRAMES; i++) refcount[i] = 0;
}

static unsigned int pmm_alloc(void)
{
	unsigned int frame = 0;
	if (free_count > 0) frame = free_list[--free_count];
	else if (next_frame + FRAME_SIZE <= end_frame) {
		frame = next_frame;
		next_frame += FRAME_SIZE;
	} else return 0;
	refcount[frame_idx(frame)] = 1;
	return frame;
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
	if (!in_range(frame)) return;
	unsigned int idx = frame_idx(frame);
	if (refcount[idx] == 0) return;
	if (--refcount[idx] > 0) return;
	if (free_count < FREE_MAX) free_list[free_count++] = frame;
}

static void pmm_ref_inc(unsigned int frame)
{
	frame &= ~(FRAME_SIZE - 1);
	if (!in_range(frame)) return;
	unsigned int idx = frame_idx(frame);
	if (refcount[idx] < 255u) refcount[idx]++;
}

static unsigned int pmm_ref(unsigned int frame)
{
	frame &= ~(FRAME_SIZE - 1);
	if (!in_range(frame)) return 0;
	return refcount[frame_idx(frame)];
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

	/* Refcount (v0.7 COW prerequisite): fresh alloc = ref 1, ref_inc bumps,
	 * free decrements + only returns to pool at 0. */
	pmm_init(0x100000, 0x110000);
	unsigned int r = pmm_alloc();
	failures += expect_uint("refcount: fresh alloc = 1",   pmm_ref(r),        1);
	pmm_ref_inc(r);
	failures += expect_uint("refcount: after ref_inc = 2", pmm_ref(r),        2);
	pmm_ref_inc(r);
	failures += expect_uint("refcount: 3rd ref = 3",       pmm_ref(r),        3);
	pmm_free(r);
	failures += expect_uint("refcount: free still held = 2", pmm_ref(r),      2);
	failures += expect_uint("refcount: not yet pooled",    pmm_free_pages(),  15);
	pmm_free(r);
	failures += expect_uint("refcount: 2nd drop = 1",      pmm_ref(r),        1);
	pmm_free(r);
	failures += expect_uint("refcount: dropped to 0",      pmm_ref(r),        0);
	failures += expect_uint("refcount: now in pool",       pmm_free_pages(),  16);
	failures += expect_uint("refcount: re-alloc returns it", pmm_alloc(),     r);
	failures += expect_uint("refcount: re-alloc resets to 1", pmm_ref(r),     1);

	/* Refcount: extra free past zero is a no-op (poison guard). */
	pmm_init(0x100000, 0x110000);
	unsigned int p = pmm_alloc();
	pmm_free(p);
	pmm_free(p);  /* double-free: must NOT corrupt the pool */
	failures += expect_uint("refcount: double free no-op", pmm_free_pages(),  16);

	/* Refcount: ref_inc/ref on out-of-range = no-op / 0. */
	failures += expect_uint("refcount: oob ref = 0",       pmm_ref(0),        0);
	pmm_ref_inc(0);  /* must not crash */

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
