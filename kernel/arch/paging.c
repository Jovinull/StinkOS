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

/* Userland framebuffer window: the physical LFB is mapped read/write into this
 * high, kernel-unused virtual range so a ring-3 app can blit straight to video
 * memory (double-buffering, no syscall per rectangle). Spans up to
 * USER_FB_PDES * 4 MiB of LFB, well clear of the pmm-managed RAM (1..32 MiB). */
#define USER_FB_BASE     0x10000000u
#define USER_FB_PDES     4u

static unsigned int *page_dir;
static unsigned int *user_pts[USER_PDES];        /* one 4 KiB PT per user PDE */
static unsigned int  user_heap_next;             /* next unmapped heap address */
static int           fb_pde_mapped;              /* 1 while the user FB PDE is live */

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
 * pointer. Also restores the FB PDE to a kernel-only identity mapping so the
 * next app cannot access the LFB until it explicitly calls SYS_MAP_FB. */
void paging_reset_user_heap(void)
{
	for (unsigned int v = USER_HEAP_LO; v < user_heap_next; v += PAGE_4KB)
		unmap_user_page(v);
	user_heap_next = USER_HEAP_LO;

	if (fb_pde_mapped) {
		unsigned int idx = USER_FB_BASE / PAGE_4MB;
		page_dir[idx] = (idx * PAGE_4MB) | PG_PRESENT | PG_RW | PG_PS;
		fb_pde_mapped = 0;
		load_cr3();
	}
}

/* Map the physical LFB at USER_FB_BASE using a single 4 MiB PSE page so
 * ring-3 apps can write directly to VRAM (zero syscalls per pixel). The
 * physical address must be 4 MiB-aligned; VBE guarantees this in practice.
 * Calling this again overwrites any prior mapping (safe for one LFB). */
void paging_map_fb(unsigned int phys_base)
{
	if (phys_base == 0)
		return;
	unsigned int idx  = USER_FB_BASE / PAGE_4MB;
	unsigned int aligned = phys_base & ~(PAGE_4MB - 1u);
	page_dir[idx] = aligned | PG_PRESENT | PG_RW | PG_USER | PG_PS;
	fb_pde_mapped = 1;
	load_cr3();
}

unsigned int paging_user_fb_base(void) { return USER_FB_BASE; }

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

unsigned int paging_user_brk(void)
{
	return user_heap_next;
}

unsigned int paging_user_mmap(unsigned int size)
{
	if (size == 0)
		return 0;
	unsigned int pages = (size + PAGE_4KB - 1u) / PAGE_4KB;
	if (user_heap_next + pages * PAGE_4KB > USER_HEAP_HI)
		return 0;

	unsigned int base = user_heap_next;
	for (unsigned int i = 0; i < pages; i++) {
		unsigned int frame = pmm_alloc();
		if (!frame) {
			/* Unwind every page we managed to allocate so the user
			 * heap doesn't end up with half-mapped reservations. */
			for (unsigned int j = 0; j < i; j++)
				unmap_user_page(base + j * PAGE_4KB);
			user_heap_next = base;
			return 0;
		}
		map_user_page(user_heap_next, frame);
		user_heap_next += PAGE_4KB;
	}
	return base;
}

int paging_user_munmap(unsigned int addr, unsigned int size)
{
	if (size == 0)
		return 0;
	if (addr < USER_HEAP_LO || addr >= USER_HEAP_HI)
		return -1;
	unsigned int pages = (size + PAGE_4KB - 1u) / PAGE_4KB;
	for (unsigned int i = 0; i < pages; i++) {
		unsigned int v = addr + i * PAGE_4KB;
		if (v >= USER_HEAP_HI)
			return -1;
		unmap_user_page(v);
	}
	return 0;
}

/* Resize the heap so the program break sits at (or just above) 'new_brk',
 * mapping or unmapping pages as needed. Sub-page requests are rounded up so
 * the break always sits on a 4 KiB boundary. Returns the resulting break --
 * equal to the requested page-aligned target on success, or a smaller value
 * if growth ran out of physical memory or hit USER_HEAP_HI. */
unsigned int paging_user_set_brk(unsigned int new_brk)
{
	if (new_brk < USER_HEAP_LO)
		new_brk = USER_HEAP_LO;
	if (new_brk > USER_HEAP_HI)
		new_brk = USER_HEAP_HI;

	unsigned int aligned = (new_brk + PAGE_4KB - 1) & ~(PAGE_4KB - 1);

	while (user_heap_next < aligned) {
		unsigned int frame = pmm_alloc();
		if (frame == 0)
			return user_heap_next;          /* OOM: partial growth */
		map_user_page(user_heap_next, frame);
		user_heap_next += PAGE_4KB;
	}
	while (user_heap_next > aligned) {
		user_heap_next -= PAGE_4KB;
		unmap_user_page(user_heap_next);
	}
	return user_heap_next;
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
