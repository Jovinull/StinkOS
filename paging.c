/* Paging. The kernel address space is a flat identity map built from 4 MiB
 * pages (PSE). The userland region spans multiple 4 MiB PDEs, each backed by
 * its own 4 KiB page table so the app's code, stack and heap can be mapped at
 * page granularity. Only those user pages carry PG_USER, so every other PDE
 * stays supervisor and a ring-3 access outside the app's region faults. The
 * code and stack are mapped upfront when the kernel boots; heap pages are
 * allocated on demand (an app that never touches the heap costs no frames). */
#include "paging.h"
#include "pmm.h"

#define PG_PRESENT 0x001
#define PG_RW      0x002
#define PG_USER    0x004
#define PG_PS      0x080          /* 4 MiB page */

#define PAGE_4MB    0x400000u
#define PAGE_4KB    0x1000u
#define ENTRIES     1024
#define FRAME_MASK  0xFFFFF000u

/* User region layout. The USER_PDES contiguous 4 MiB PDEs starting at
 * USER_BASE form a 16 MiB virtual range, carved into:
 *   CODE+DATA+BSS : [USER_CODE,   USER_CODE_END)   1 MiB  (256 pages)
 *   STACK         : [USER_STACK_LO, USER_STACK_TOP) 256 KiB (64 pages)
 *   HEAP          : [USER_HEAP_LO, USER_HEAP_HI)   ~14 MiB (grows lazily)
 * The stack grows down from USER_STACK_TOP; the heap grows up from
 * USER_HEAP_LO via paging_user_alloc. */
#define USER_BASE        0x400000u
#define USER_PDES        4u
#define USER_END         (USER_BASE + USER_PDES * PAGE_4MB)

#define USER_CODE        USER_BASE
#define USER_CODE_PAGES  256u
#define USER_CODE_END    (USER_CODE + USER_CODE_PAGES * PAGE_4KB)

#define USER_STACK_PAGES 64u
#define USER_STACK_LO    USER_CODE_END
#define USER_STACK_TOP   (USER_STACK_LO + USER_STACK_PAGES * PAGE_4KB)

#define USER_HEAP_LO     USER_STACK_TOP
#define USER_HEAP_HI     USER_END

static unsigned int *page_dir;
static unsigned int *user_pts[USER_PDES];        /* one 4 KiB PT per user PDE */
static unsigned int  user_heap_next;             /* next unmapped heap address */

static void load_cr3(void)
{
	__asm__ volatile ("mov %0, %%cr3" : : "r"((unsigned int)page_dir) : "memory");
}

void paging_init(void)
{
	page_dir = (unsigned int *)pmm_alloc();

	for (unsigned int i = 0; i < ENTRIES; i++)
		page_dir[i] = (i * PAGE_4MB) | PG_PRESENT | PG_RW | PG_PS;

	unsigned int cr4;
	__asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
	cr4 |= 0x10;                                        /* CR4.PSE */
	__asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));

	load_cr3();

	unsigned int cr0;
	__asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
	cr0 |= 0x80000000;                                  /* CR0.PG */
	__asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));
}

/* Locate the PTE backing 'vaddr' within the user range. Returns 0 outside it. */
static unsigned int *user_pte(unsigned int vaddr)
{
	if (vaddr < USER_BASE || vaddr >= USER_END)
		return 0;
	unsigned int pde = (vaddr - USER_BASE) / PAGE_4MB;
	return &user_pts[pde][(vaddr >> 12) & 0x3FF];
}

static void map_user_page(unsigned int vaddr, unsigned int frame)
{
	unsigned int *pte = user_pte(vaddr);
	*pte = (frame & FRAME_MASK) | PG_PRESENT | PG_RW | PG_USER;
}

/* Tear down the mapping at 'vaddr' and release its physical frame back to the
 * PMM. Invalidates the stale TLB entry so the next access fresh-walks. */
static void unmap_user_page(unsigned int vaddr)
{
	unsigned int *pte = user_pte(vaddr);
	if (!pte || !(*pte & PG_PRESENT))
		return;
	unsigned int frame = *pte & FRAME_MASK;
	*pte = 0;
	pmm_free(frame);
	__asm__ volatile ("invlpg (%0)" : : "r"(vaddr) : "memory");
}

/* Build the userland address space: allocate one 4 KiB page table per user PDE,
 * point each PDE at it (clearing PG_PS so it is read as a page table, not a
 * 4 MiB page), and map the code + stack pages. The heap is left unmapped --
 * paging_user_alloc grows it on demand the first time an app touches a page. */
void paging_init_user(void)
{
	for (unsigned int p = 0; p < USER_PDES; p++) {
		unsigned int *pt = (unsigned int *)pmm_alloc();
		for (unsigned int i = 0; i < ENTRIES; i++)
			pt[i] = 0;
		user_pts[p] = pt;
		page_dir[(USER_BASE / PAGE_4MB) + p] =
			(unsigned int)pt | PG_PRESENT | PG_RW | PG_USER;
	}

	for (unsigned int i = 0; i < USER_CODE_PAGES; i++)
		map_user_page(USER_CODE + i * PAGE_4KB, pmm_alloc());
	for (unsigned int i = 0; i < USER_STACK_PAGES; i++)
		map_user_page(USER_STACK_LO + i * PAGE_4KB, pmm_alloc());

	user_heap_next = USER_HEAP_LO;
	load_cr3();
}

unsigned int paging_user_code(void)       { return USER_CODE; }
unsigned int paging_user_code_end(void)   { return USER_CODE_END; }
unsigned int paging_user_stack_top(void)  { return USER_STACK_TOP; }

/* Release every heap page the previous app mapped, then rewind the bump
 * pointer. The next app sees a fresh, empty heap. */
void paging_reset_user_heap(void)
{
	for (unsigned int v = USER_HEAP_LO; v < user_heap_next; v += PAGE_4KB)
		unmap_user_page(v);
	user_heap_next = USER_HEAP_LO;
}

/* Lazily map and return the next 4 KiB heap page (a virtual address), or 0 if
 * the heap is full or physical memory is exhausted. */
unsigned int paging_user_alloc(void)
{
	if (user_heap_next >= USER_HEAP_HI)
		return 0;
	unsigned int frame = pmm_alloc();
	if (frame == 0)
		return 0;
	unsigned int v = user_heap_next;
	map_user_page(v, frame);
	user_heap_next += PAGE_4KB;
	return v;
}

/* Validate a userland buffer before the kernel dereferences it: the range must
 * sit wholly inside one mapped span -- code+stack (contiguous) or the portion
 * of the heap that has actually been mapped so far. */
int paging_user_range_ok(unsigned int addr, unsigned int len)
{
	if (len == 0)
		return 1;
	if (addr + len < addr)                              /* address overflow */
		return 0;

	unsigned int end = addr + len;
	if (addr >= USER_CODE && end <= USER_STACK_TOP)
		return 1;
	if (addr >= USER_HEAP_LO && end <= user_heap_next)
		return 1;
	return 0;
}
