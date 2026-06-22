/* Identity-maps the full 4 GiB address space using 4 MiB pages (PSE), so the
 * kernel, low memory and the linear framebuffer keep their physical addresses
 * once paging is on. Per-process address spaces come later, for userland. */
#include "paging.h"
#include "pmm.h"

#define PG_PRESENT 0x001
#define PG_RW      0x002
#define PG_USER    0x004
#define PG_PS      0x080          /* 4 MiB page */

#define PAGE_4MB   0x400000u
#define ENTRIES    1024

static unsigned int *page_dir;

void paging_init(void)
{
	page_dir = (unsigned int *)pmm_alloc();             /* 4 KiB-aligned frame */

	for (unsigned int i = 0; i < ENTRIES; i++)
		page_dir[i] = (i * PAGE_4MB) | PG_PRESENT | PG_RW | PG_PS;

	unsigned int cr4;
	__asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
	cr4 |= 0x10;                                        /* CR4.PSE */
	__asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));

	__asm__ volatile ("mov %0, %%cr3" : : "r"((unsigned int)page_dir));

	unsigned int cr0;
	__asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
	cr0 |= 0x80000000;                                  /* CR0.PG */
	__asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));
}

/* Mark the 4 MiB region containing addr as user-accessible (ring 3).
 * Interim: whole-region access until per-process 4 KiB page tables exist. */
void paging_set_user(unsigned int addr)
{
	page_dir[addr / PAGE_4MB] |= PG_USER;
	__asm__ volatile ("mov %0, %%cr3" : : "r"((unsigned int)page_dir) : "memory");
}
