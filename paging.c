/* Identity-maps the full 4 GiB address space using 4 MiB pages (PSE), so the
 * kernel, low memory and the linear framebuffer keep their physical addresses
 * once paging is on. Per-process address spaces come later, for userland. */
#include "paging.h"
#include "pmm.h"

#define PG_PRESENT 0x001
#define PG_RW      0x002
#define PG_PS      0x080          /* 4 MiB page */

#define PAGE_4MB   0x400000u
#define ENTRIES    1024

void paging_init(void)
{
	unsigned int *dir = (unsigned int *)pmm_alloc();   /* 4 KiB-aligned frame */

	for (unsigned int i = 0; i < ENTRIES; i++)
		dir[i] = (i * PAGE_4MB) | PG_PRESENT | PG_RW | PG_PS;

	unsigned int cr4;
	__asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
	cr4 |= 0x10;                                        /* CR4.PSE */
	__asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));

	__asm__ volatile ("mov %0, %%cr3" : : "r"((unsigned int)dir));

	unsigned int cr0;
	__asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
	cr0 |= 0x80000000;                                  /* CR0.PG */
	__asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));
}
