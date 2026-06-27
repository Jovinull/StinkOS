/* Host-side test for the size overflow guard in
 * kernel/arch/paging.c (paging_user_mmap / paging_user_munmap).
 * The page-round-up `(size + PAGE_4KB - 1) / PAGE_4KB` wraps to
 * tiny page counts when size is close to 0xFFFFFFFF; a hostile
 * userland could otherwise pass size=0xFFFFFFFF, sneak past a naive
 * upper-bound check, and pull the kernel into a million-page mapping
 * loop.
 *
 * The fix is a room-cap before the round-up: clamp `size > room` to
 * a fail. This test mirrors that decision: success returns the base
 * address (non-zero) or 0 on reject; on munmap, 0 = ok / -1 = reject.
 */
#include <stdio.h>

#define PAGE_4KB     0x1000u
#define USER_HEAP_LO 0x40000000u
#define USER_HEAP_HI 0x80000000u

static unsigned int user_heap_next;

/* Mirror of paging_user_mmap -- only the guard arithmetic, the actual
 * pmm_alloc / map_user_page are stubbed out. Returns the would-be base
 * on success or 0 on reject. */
static unsigned int sim_user_mmap(unsigned int size)
{
	if (size == 0) return 0;
	unsigned int room = USER_HEAP_HI - user_heap_next;
	if (size > room) return 0;
	unsigned int pages = (size + PAGE_4KB - 1u) / PAGE_4KB;
	if (user_heap_next + pages * PAGE_4KB > USER_HEAP_HI) return 0;
	unsigned int base = user_heap_next;
	user_heap_next += pages * PAGE_4KB;
	return base;
}

static int sim_user_munmap(unsigned int addr, unsigned int size)
{
	if (size == 0) return 0;
	if (addr < USER_HEAP_LO || addr >= USER_HEAP_HI) return -1;
	unsigned int room = USER_HEAP_HI - addr;
	if (size > room) return -1;
	unsigned int pages = (size + PAGE_4KB - 1u) / PAGE_4KB;
	for (unsigned int i = 0; i < pages; i++)
		if (addr + i * PAGE_4KB >= USER_HEAP_HI) return -1;
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

	/* Fresh heap. Tiny request: gets the base of the user heap. */
	user_heap_next = USER_HEAP_LO;
	failures += expect_u("first mmap(4096) -> USER_HEAP_LO",
	                     sim_user_mmap(PAGE_4KB), USER_HEAP_LO);

	/* Next call advances by 1 page. */
	failures += expect_u("second mmap(4096) -> +4 KiB",
	                     sim_user_mmap(PAGE_4KB), USER_HEAP_LO + PAGE_4KB);

	/* size=0 always rejects. */
	failures += expect_u("mmap(0) -> reject",
	                     sim_user_mmap(0), 0);

	/* Overflow attack: size == 0xFFFFFFFF. Without the guard,
	 * (size + 0xFFF) / 0x1000 wraps to 0, allocator runs zero pages
	 * and returns a valid base while leaving the bookkeeping wrong.
	 * The guard rejects via the room check. */
	user_heap_next = USER_HEAP_LO;
	failures += expect_u("mmap(0xFFFFFFFF) -> reject",
	                     sim_user_mmap(0xFFFFFFFFu), 0);

	/* size just above room: reject. */
	user_heap_next = USER_HEAP_HI - PAGE_4KB;
	failures += expect_u("mmap(8192) at room=4096 -> reject",
	                     sim_user_mmap(2u * PAGE_4KB), 0);

	/* size exactly at room: accept and return base. */
	user_heap_next = USER_HEAP_HI - PAGE_4KB;
	failures += expect_u("mmap(4096) at room=4096 -> accept",
	                     sim_user_mmap(PAGE_4KB), USER_HEAP_HI - PAGE_4KB);

	/* munmap: size=0 is a no-op. */
	failures += expect_int("munmap(_, 0) -> 0",
	                       sim_user_munmap(USER_HEAP_LO, 0), 0);

	/* munmap outside the user heap window: reject. */
	failures += expect_int("munmap(below LO) -> -1",
	                       sim_user_munmap(USER_HEAP_LO - PAGE_4KB, PAGE_4KB), -1);
	failures += expect_int("munmap(>= HI) -> -1",
	                       sim_user_munmap(USER_HEAP_HI, PAGE_4KB), -1);

	/* munmap overflow attack: addr just below HI, size = 0xFFFFFFFF.
	 * The guard's room check rejects before the round-up wraps. */
	failures += expect_int("munmap(LO, 0xFFFFFFFF) -> -1",
	                       sim_user_munmap(USER_HEAP_LO, 0xFFFFFFFFu), -1);

	/* Legitimate munmap inside the window. */
	failures += expect_int("munmap(LO, 8192) -> 0",
	                       sim_user_munmap(USER_HEAP_LO, 2u * PAGE_4KB), 0);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
