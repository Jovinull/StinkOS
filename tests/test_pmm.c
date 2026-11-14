/* Host-side test for the physical memory manager.
 *
 * Links the real kernel/arch/pmm.c (not a copy of its logic): the allocator
 * only does arithmetic on physical addresses and never dereferences a
 * frame, so it runs unchanged on the host. The only stubs are the two
 * serial routines its OOM message uses.
 *
 * Invariants locked here:
 *   - alignment + bounds of handed-out frames, watermark never overruns
 *   - a freed frame is reusable, and free/total accounting stays exact
 *   - releasing more frames than any fixed-size free list could hold
 *     returns every one of them (regression: the list used to cap at 1024
 *     entries and silently dropped the rest)
 *   - no frame is ever handed out twice while still owned
 *   - refcounts: alloc = 1, ref_inc shares, free only pools at zero,
 *     double free and out-of-range free are no-ops
 */
#include <stdio.h>
#include <string.h>
#include "pmm.h"
#include "memlayout.h"

void serial_write(const char *s)        { (void)s; }
void serial_write_dec(unsigned int v)   { (void)v; }

#define FRAME 4096u

static int failures;

static void expect(const char *label, unsigned int got, unsigned int want)
{
	if (got == want) {
		printf("ok   %-56s = %u\n", label, got);
	} else {
		printf("FAIL %s: got %u, want %u\n", label, got, want);
		failures++;
	}
}

int main(void)
{
	/* ---- small range: basic alloc / free / OOM ---- */
	pmm_init(0x100000, 0x110000);                 /* 16 frames */
	expect("init: total = 16 pages", pmm_total_pages(), 16);
	expect("init: free = 16 pages", pmm_free_pages(), 16);

	unsigned int a = pmm_alloc();
	expect("alloc: aligned", a & (FRAME - 1), 0);
	expect("alloc: inside range", a >= 0x100000 && a < 0x110000, 1);
	expect("alloc: free = 15", pmm_free_pages(), 15);
	expect("alloc: refcount = 1", pmm_ref(a), 1);
	for (int i = 0; i < 15; i++)
		(void)pmm_alloc();
	expect("exhausted: free = 0", pmm_free_pages(), 0);
	expect("exhausted: alloc returns 0", pmm_alloc(), 0);

	pmm_free(a);
	expect("free: free = 1", pmm_free_pages(), 1);
	expect("free: refcount = 0", pmm_ref(a), 0);
	expect("re-alloc returns the freed frame", pmm_alloc(), a);

	/* ---- bogus frees are ignored ---- */
	pmm_init(0x100000, 0x110000);
	for (int i = 0; i < 16; i++)
		(void)pmm_alloc();
	pmm_free(0);
	pmm_free(0x000FF000);                          /* below the range */
	pmm_free(0x00200000);                          /* above the range */
	expect("bogus frees: nothing pooled", pmm_free_pages(), 0);
	pmm_free(0x0010FFFF);                          /* misaligned: rounds down */
	expect("misaligned free: one frame pooled", pmm_free_pages(), 1);
	pmm_free(0x0010F000);                          /* same frame again */
	expect("double free: still one frame pooled", pmm_free_pages(), 1);

	/* A frame above the watermark was never handed out: not freeable. */
	pmm_init(0x100000, 0x110000);
	(void)pmm_alloc();
	pmm_free(0x00108000);
	expect("free of never-allocated frame ignored", pmm_free_pages(), 15);

	/* ---- unaligned init bounds round inward ---- */
	pmm_init(0x100123, 0x103FFF);                 /* -> [0x101000, 0x103000) */
	expect("unaligned init: total = 2", pmm_total_pages(), 2);
	expect("unaligned init: first frame", pmm_alloc(), 0x101000);
	expect("unaligned init: second frame", pmm_alloc(), 0x102000);
	expect("unaligned init: then OOM", pmm_alloc(), 0);

	/* ---- bulk release: every frame comes back ---- */
	enum { N = 6000 };
	static unsigned int got[N];
	unsigned int limit = PMM_PHYS_LIMIT;
	pmm_init(0x100000, limit);
	unsigned int total = pmm_total_pages();
	expect("bulk: range holds at least 6000 frames", total >= N, 1);
	for (int i = 0; i < N; i++)
		got[i] = pmm_alloc();
	expect("bulk: free after 6000 allocs", pmm_free_pages(), total - N);
	for (int i = 0; i < N; i++)
		pmm_free(got[i]);
	expect("bulk: all 6000 frames returned", pmm_free_pages(), total);

	/* ...and every one of them is allocatable again, each exactly once. */
	static unsigned char seen[PMM_PHYS_LIMIT / FRAME];
	memset(seen, 0, sizeof(seen));
	unsigned int dup = 0, handed = 0;
	for (;;) {
		unsigned int f = pmm_alloc();
		if (!f)
			break;
		unsigned int idx = f / FRAME;
		if (seen[idx])
			dup++;
		seen[idx] = 1;
		handed++;
	}
	expect("bulk: re-alloc hands out the whole range", handed, total);
	expect("bulk: no frame handed out twice", dup, 0);

	/* ---- interleaved churn keeps accounting exact ---- */
	pmm_init(0x100000, 0x100000 + 64 * FRAME);
	unsigned int held[64];
	int nheld = 0;
	unsigned int rng = 12345;
	for (int step = 0; step < 20000; step++) {
		rng = rng * 1103515245u + 12345u;
		if ((rng >> 16) & 1) {
			unsigned int f = pmm_alloc();
			if (f)
				held[nheld++] = f;
			else if (nheld != 64)
				failures++;
		} else if (nheld > 0) {
			int k = (int)((rng >> 8) % (unsigned int)nheld);
			pmm_free(held[k]);
			held[k] = held[--nheld];
		}
		if (pmm_free_pages() != 64u - (unsigned int)nheld) {
			printf("FAIL churn: accounting drifted at step %d\n", step);
			failures++;
			break;
		}
	}
	expect("churn: accounting exact after 20000 ops",
	       pmm_free_pages(), 64u - (unsigned int)nheld);

	/* ---- refcounts (COW sharing) ---- */
	pmm_init(0x100000, 0x110000);
	unsigned int s = pmm_alloc();
	pmm_ref_inc(s);
	pmm_ref_inc(s);
	expect("share: refcount = 3", pmm_ref(s), 3);
	pmm_free(s);
	expect("share: first free keeps the frame", pmm_free_pages(), 15);
	pmm_free(s);
	expect("share: second free keeps the frame", pmm_free_pages(), 15);
	pmm_free(s);
	expect("share: last free pools the frame", pmm_free_pages(), 16);
	pmm_free(s);
	expect("share: extra free is a no-op", pmm_free_pages(), 16);
	pmm_ref_inc(0);
	pmm_ref_inc(0x00200000);
	expect("ref_inc out of range is a no-op", pmm_ref(0x00200000), 0);

	/* ---- init never manages more than the tables describe ---- */
	pmm_init(0x100000, 0xF0000000u);
	expect("oversized range clamped to PMM_PHYS_LIMIT worth of frames",
	       pmm_total_pages(), PMM_PHYS_LIMIT / FRAME);

	printf("\n%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
