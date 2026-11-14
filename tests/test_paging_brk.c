/* Host-side model of the program-break contract in kernel/arch/paging.c
 * (paging_user_set_brk) and of the arithmetic SYS_SBRK wraps around it.
 *
 * paging.c itself cannot be linked on the host (privileged instructions,
 * the kernel direct map), so this file mirrors the rule and checks its
 * corner cases cheaply. The real code path is exercised end to end by the
 * ktest cases `heap-switch`, `sbrk-oom`, `munmap-hole` and `badptr`
 * (tools/qa/ktest.py) -- treat those as the authority; this is the fast
 * arithmetic check.
 *
 * Contract:
 *   - the target is rounded UP to a page
 *   - a target outside [USER_HEAP_LO, USER_HEAP_HI] is refused
 *   - growing is all-or-nothing: if frames run out half way, every page
 *     mapped by the call is released and the break does not move
 *   - shrinking returns one frame per page dropped
 */
#include <stdio.h>

#define PAGE_4KB         0x1000u
#define USER_HEAP_LO     0x00540000u
#define USER_HEAP_HI     0x01400000u

static unsigned int user_heap_next;
static int          frames_left;            /* stand-in for the PMM */

static int set_brk(unsigned int new_brk)
{
	if (new_brk < USER_HEAP_LO || new_brk > USER_HEAP_HI)
		return -1;
	unsigned int aligned = (new_brk + PAGE_4KB - 1) & ~(PAGE_4KB - 1);
	unsigned int start = user_heap_next;
	while (user_heap_next < aligned) {
		if (frames_left == 0) {
			while (user_heap_next > start) {
				user_heap_next -= PAGE_4KB;
				frames_left++;
			}
			return -1;
		}
		frames_left--;
		user_heap_next += PAGE_4KB;
	}
	while (user_heap_next > aligned) {
		user_heap_next -= PAGE_4KB;
		frames_left++;
	}
	return 0;
}

/* SYS_SBRK: signed delta -> old break, or -1. */
static unsigned int sbrk(int delta)
{
	unsigned int old = user_heap_next;
	unsigned int target;
	if (delta >= 0) {
		target = old + (unsigned int)delta;
		if (target < old)
			return (unsigned int)-1;
	} else {
		unsigned int abs_delta = (unsigned int)(-delta);
		if (abs_delta > old)
			return (unsigned int)-1;
		target = old - abs_delta;
	}
	return set_brk(target) != 0 ? (unsigned int)-1 : old;
}

static int failures;

static void expect_u(const char *label, unsigned int got, unsigned int want)
{
	if (got == want) {
		printf("ok   %-58s = 0x%x\n", label, got);
	} else {
		printf("FAIL %s: got 0x%x, want 0x%x\n", label, got, want);
		failures++;
	}
}

int main(void)
{
	user_heap_next = USER_HEAP_LO;
	frames_left = 1000;

	expect_u("below the heap is refused", (unsigned int)set_brk(USER_HEAP_LO - 1), (unsigned int)-1);
	expect_u("above the heap is refused", (unsigned int)set_brk(USER_HEAP_HI + 1), (unsigned int)-1);
	expect_u("refused: break unchanged", user_heap_next, USER_HEAP_LO);
	expect_u("refused: no frame consumed", (unsigned int)frames_left, 1000);

	expect_u("LO+1 rounds up to one page", (unsigned int)set_brk(USER_HEAP_LO + 1), 0);
	expect_u("  break", user_heap_next, USER_HEAP_LO + PAGE_4KB);
	expect_u("LO+4096 is still one page", (unsigned int)set_brk(USER_HEAP_LO + 4096), 0);
	expect_u("  break", user_heap_next, USER_HEAP_LO + PAGE_4KB);
	expect_u("LO+4097 is two pages", (unsigned int)set_brk(USER_HEAP_LO + 4097), 0);
	expect_u("  break", user_heap_next, USER_HEAP_LO + 2 * PAGE_4KB);
	expect_u("  frames left", (unsigned int)frames_left, 998);

	expect_u("shrink to LO", (unsigned int)set_brk(USER_HEAP_LO), 0);
	expect_u("  frames all returned", (unsigned int)frames_left, 1000);

	/* Out of memory half way through a grow: nothing sticks. */
	user_heap_next = USER_HEAP_LO + 3 * PAGE_4KB;
	frames_left = 2;
	expect_u("grow 5 pages with 2 frames fails", (unsigned int)set_brk(USER_HEAP_LO + 8 * PAGE_4KB), (unsigned int)-1);
	expect_u("  break did not move", user_heap_next, USER_HEAP_LO + 3 * PAGE_4KB);
	expect_u("  both frames given back", (unsigned int)frames_left, 2);

	/* The exact top of the heap is reachable, one byte more is not. */
	user_heap_next = USER_HEAP_LO;
	frames_left = 100000;
	expect_u("grow to exactly HI", (unsigned int)set_brk(USER_HEAP_HI), 0);
	expect_u("  break == HI", user_heap_next, USER_HEAP_HI);

	/* SYS_SBRK arithmetic. */
	user_heap_next = USER_HEAP_LO;
	frames_left = 1000;
	expect_u("sbrk(0) returns the break", sbrk(0), USER_HEAP_LO);
	expect_u("sbrk(8192) returns the old break", sbrk(8192), USER_HEAP_LO);
	expect_u("  break advanced", user_heap_next, USER_HEAP_LO + 2 * PAGE_4KB);
	expect_u("sbrk(-4096) returns the old break", sbrk(-4096), USER_HEAP_LO + 2 * PAGE_4KB);
	expect_u("  break dropped a page", user_heap_next, USER_HEAP_LO + PAGE_4KB);
	expect_u("sbrk below the heap base fails", sbrk(-8192), (unsigned int)-1);
	expect_u("  break unchanged", user_heap_next, USER_HEAP_LO + PAGE_4KB);
	expect_u("absurd sbrk fails", sbrk(0x7FF00000), (unsigned int)-1);
	expect_u("  break unchanged", user_heap_next, USER_HEAP_LO + PAGE_4KB);
	expect_u("  no frame consumed", (unsigned int)frames_left, 999);

	printf("\n%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
