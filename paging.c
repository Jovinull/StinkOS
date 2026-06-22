/* Paging. The kernel address space is a flat identity map built from 4 MiB
 * pages (PSE). The userland region (USER_BASE) is instead backed by a 4 KiB
 * page table whose only present entries are the app's code, stack and heap --
 * all marked user. Every other PDE stays supervisor, so a ring-3 app is
 * confined to its own region and faults if it touches kernel memory. */
#include "paging.h"
#include "pmm.h"

#define PG_PRESENT 0x001
#define PG_RW      0x002
#define PG_USER    0x004
#define PG_PS      0x080          /* 4 MiB page */

#define PAGE_4MB   0x400000u
#define PAGE_4KB   0x1000u
#define ENTRIES    1024

/* User region layout (all inside one 4 MiB PDE). */
#define USER_BASE   0x400000u
#define USER_CODE   0x400000u     /* 2 pages of app code           */
#define USER_STACK  0x402000u     /* 1 page stack (top = 0x403000) */
#define USER_HEAP   0x404000u     /* heap pages handed out by SysAlloc */
#define USER_CODE_PAGES 2
#define USER_HEAP_PAGES 8

static unsigned int *page_dir;
static unsigned int  user_heap_next;

static void load_cr3(void)
{
	__asm__ volatile ("mov %0, %%cr3" : : "r"((unsigned int)page_dir) : "memory");
}

void paging_init(void)
{
	page_dir = (unsigned int *)pmm_alloc();             /* 4 KiB-aligned frame */

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

/* Build the userland address space: a 4 KiB page table for the USER_BASE region
 * with the app's code/stack/heap pages mapped present+user, then point the PDE
 * at it (clearing PG_PS so it is read as a page table, not a 4 MiB page). */
void paging_init_user(void)
{
	unsigned int *pt = (unsigned int *)pmm_alloc();
	for (unsigned int i = 0; i < ENTRIES; i++)
		pt[i] = 0;                                  /* not present */

	for (unsigned int i = 0; i < USER_CODE_PAGES; i++)
		pt[((USER_CODE >> 12) & 0x3FF) + i] =
			pmm_alloc() | PG_PRESENT | PG_RW | PG_USER;

	pt[(USER_STACK >> 12) & 0x3FF] =
		pmm_alloc() | PG_PRESENT | PG_RW | PG_USER;

	for (unsigned int i = 0; i < USER_HEAP_PAGES; i++)
		pt[((USER_HEAP >> 12) & 0x3FF) + i] =
			pmm_alloc() | PG_PRESENT | PG_RW | PG_USER;

	page_dir[USER_BASE / PAGE_4MB] =
		(unsigned int)pt | PG_PRESENT | PG_RW | PG_USER;

	user_heap_next = USER_HEAP;
	load_cr3();
}

unsigned int paging_user_code(void)       { return USER_CODE; }
unsigned int paging_user_stack_top(void)  { return USER_STACK + PAGE_4KB; }

void paging_reset_user_heap(void)
{
	user_heap_next = USER_HEAP;
}

/* Hand out the next pre-mapped user heap page (a virtual address), or 0. */
unsigned int paging_user_alloc(void)
{
	if (user_heap_next >= USER_HEAP + USER_HEAP_PAGES * PAGE_4KB)
		return 0;
	unsigned int v = user_heap_next;
	user_heap_next += PAGE_4KB;
	return v;
}
