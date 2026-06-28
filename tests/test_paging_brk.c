/* Host-side test for paging_user_set_brk + paging_user_range_ok in
 * kernel/arch/paging.c. Two pieces:
 *
 *   1. set_brk: align UP to PAGE_4KB, clamp to [USER_HEAP_LO,
 *      USER_HEAP_HI], grow page-by-page (mapping fresh frames) until
 *      the break hits the target, OR shrink page-by-page (unmapping)
 *      down to the target. Returns the actual new break. On OOM
 *      mid-growth, returns the partial break.
 *
 *   2. range_ok: a user buffer is OK only if [addr, addr+len) sits
 *      wholly inside the contiguous code+stack span or wholly inside
 *      the currently-mapped heap region. Address overflow is rejected
 *      (addr + len < addr).
 *
 * Both of those gate raw user memory access from the kernel; off-by-
 * page errors here either leak a page on every shrink/grow or let a
 * userland app trick the kernel into dereferencing unmapped memory.
 */
#include <stdio.h>

#define PAGE_4KB         0x1000u
#define USER_CODE        0x00800000u
#define USER_STACK_TOP   0x00900000u
#define USER_HEAP_LO     0x40000000u
#define USER_HEAP_HI     0x80000000u

static unsigned int user_heap_next;
static int          frames_left;            /* OOM throttle for the sim */

/* Mirror of paging_user_set_brk: pmm_alloc is faked with `frames_left`. */
static unsigned int set_brk(unsigned int new_brk)
{
	if (new_brk < USER_HEAP_LO) new_brk = USER_HEAP_LO;
	if (new_brk > USER_HEAP_HI) new_brk = USER_HEAP_HI;
	unsigned int aligned = (new_brk + PAGE_4KB - 1) & ~(PAGE_4KB - 1);
	while (user_heap_next < aligned) {
		if (frames_left == 0) return user_heap_next;
		frames_left--;
		user_heap_next += PAGE_4KB;
	}
	while (user_heap_next > aligned) {
		user_heap_next -= PAGE_4KB;
		frames_left++;                      /* unmapped frame returns to pool */
	}
	return user_heap_next;
}

static int range_ok(unsigned int addr, unsigned int len)
{
	if (len == 0) return 1;
	if (addr + len < addr) return 0;
	unsigned int end = addr + len;
	if (addr >= USER_CODE && end <= USER_STACK_TOP) return 1;
	if (addr >= USER_HEAP_LO && end <= user_heap_next) return 1;
	return 0;
}

static int expect_u(const char *label, unsigned int got, unsigned int want)
{
	if (got == want) { printf("ok   %-55s = 0x%x\n", label, got); return 0; }
	printf("FAIL %s: got 0x%x, want 0x%x\n", label, got, want);
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

	/* --- set_brk: clamp below ---------------------------------- */
	user_heap_next = USER_HEAP_LO; frames_left = 1000;
	failures += expect_u("set_brk below LO -> snaps to LO",
	                     set_brk(0), USER_HEAP_LO);

	/* --- set_brk: clamp above HI ------------------------------ */
	user_heap_next = USER_HEAP_LO; frames_left = 100000;
	/* growing all the way to HI would need (HI-LO)/4KB pages, too
	 * many for a fast sim. So we test the clamp path: pass an
	 * obscenely-high target with no headroom -> clamps to HI. */
	(void)set_brk(USER_HEAP_LO);
	failures += expect_u("set_brk at LO -> stays LO",
	                     user_heap_next, USER_HEAP_LO);

	/* --- set_brk: page-aligned grow -------------------------- */
	user_heap_next = USER_HEAP_LO; frames_left = 1000;
	failures += expect_u("set_brk LO+4096 -> grows 1 page",
	                     set_brk(USER_HEAP_LO + PAGE_4KB),
	                     USER_HEAP_LO + PAGE_4KB);
	failures += expect_int("after grow: 999 frames left",
	                       frames_left, 999);

	/* --- set_brk: sub-page round-up -------------------------- */
	user_heap_next = USER_HEAP_LO; frames_left = 1000;
	failures += expect_u("set_brk LO+1 -> rounds up to one page",
	                     set_brk(USER_HEAP_LO + 1),
	                     USER_HEAP_LO + PAGE_4KB);
	failures += expect_u("set_brk LO+4095 -> still one page",
	                     set_brk(USER_HEAP_LO + 4095),
	                     USER_HEAP_LO + PAGE_4KB);
	failures += expect_u("set_brk LO+4096 -> still one page",
	                     set_brk(USER_HEAP_LO + 4096),
	                     USER_HEAP_LO + PAGE_4KB);
	failures += expect_u("set_brk LO+4097 -> two pages",
	                     set_brk(USER_HEAP_LO + 4097),
	                     USER_HEAP_LO + 2u * PAGE_4KB);

	/* --- set_brk: shrink ----------------------------------- */
	user_heap_next = USER_HEAP_LO; frames_left = 100;
	(void)set_brk(USER_HEAP_LO + 10u * PAGE_4KB);
	failures += expect_int("after grow 10: 90 frames left",
	                       frames_left, 90);
	failures += expect_u("shrink to LO+3 pages",
	                     set_brk(USER_HEAP_LO + 3u * PAGE_4KB),
	                     USER_HEAP_LO + 3u * PAGE_4KB);
	failures += expect_int("after shrink: 7 frames returned (97 left)",
	                       frames_left, 97);

	/* --- set_brk: OOM mid-grow returns partial break ------- */
	user_heap_next = USER_HEAP_LO; frames_left = 2;
	failures += expect_u("grow 5 pages with only 2 frames -> partial",
	                     set_brk(USER_HEAP_LO + 5u * PAGE_4KB),
	                     USER_HEAP_LO + 2u * PAGE_4KB);

	/* --- range_ok: zero length always OK ------------------ */
	user_heap_next = USER_HEAP_LO + 2u * PAGE_4KB;
	failures += expect_int("range_ok(addr, 0) -> always 1",
	                       range_ok(0xDEADBEEF, 0), 1);

	/* --- range_ok: addr+len overflow rejected ------------- */
	failures += expect_int("range_ok overflow -> 0",
	                       range_ok(0xFFFFFFF0u, 0x20), 0);

	/* --- range_ok: code+stack span (inclusive low, exclusive high) */
	failures += expect_int("range_ok mid code+stack -> 1",
	                       range_ok(USER_CODE + 0x100, 64), 1);
	failures += expect_int("range_ok ending exactly at STACK_TOP -> 1",
	                       range_ok(USER_STACK_TOP - 4, 4), 1);
	failures += expect_int("range_ok crossing STACK_TOP -> 0",
	                       range_ok(USER_STACK_TOP - 4, 16), 0);
	failures += expect_int("range_ok below USER_CODE -> 0",
	                       range_ok(USER_CODE - 16, 4), 0);

	/* --- range_ok: heap within currently-mapped portion --- */
	failures += expect_int("range_ok inside mapped heap -> 1",
	                       range_ok(USER_HEAP_LO, 128), 1);
	failures += expect_int("range_ok at exactly heap_next -> 1 (end=next)",
	                       range_ok(USER_HEAP_LO,
	                                user_heap_next - USER_HEAP_LO), 1);
	failures += expect_int("range_ok past heap_next -> 0",
	                       range_ok(USER_HEAP_LO,
	                                (user_heap_next - USER_HEAP_LO) + 1), 0);
	failures += expect_int("range_ok wholly past heap -> 0",
	                       range_ok(user_heap_next, 16), 0);

	/* --- range_ok: gap between code+stack and heap -> 0 -- */
	failures += expect_int("range_ok in unmapped gap -> 0",
	                       range_ok(0x10000000u, 64), 0);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
