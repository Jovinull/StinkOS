/* Builds a flat GDT: null, kernel code/data (ring 0), user code/data (ring 3)
 * and a TSS so ring 3 can trap back into ring 0 on a known kernel stack. */
#include "gdt.h"

extern void gdt_flush(unsigned int gdt_ptr_addr);
extern void tss_flush(void);

struct gdt_entry {
	unsigned short limit_lo;
	unsigned short base_lo;
	unsigned char  base_mid;
	unsigned char  access;
	unsigned char  gran;
	unsigned char  base_hi;
} __attribute__((packed));

struct gdt_ptr {
	unsigned short limit;
	unsigned int   base;
} __attribute__((packed));

struct tss_entry {
	unsigned int   prev_tss;
	unsigned int   esp0;
	unsigned int   ss0;
	unsigned int   esp1, ss1, esp2, ss2;
	unsigned int   cr3, eip, eflags;
	unsigned int   eax, ecx, edx, ebx, esp, ebp, esi, edi;
	unsigned int   es, cs, ss, ds, fs, gs;
	unsigned int   ldt;
	unsigned short trap;
	unsigned short iomap_base;
} __attribute__((packed));

static struct gdt_entry gdt[6];
static struct gdt_ptr   gp;
static struct tss_entry tss;

/* Ring-0 stack used when a ring-3 task traps into the kernel. */
static unsigned char kernel_stack[8192];

static void set_gate(int n, unsigned int base, unsigned int limit,
                     unsigned char access, unsigned char gran)
{
	gdt[n].base_lo  = base & 0xFFFF;
	gdt[n].base_mid = (base >> 16) & 0xFF;
	gdt[n].base_hi  = (base >> 24) & 0xFF;
	gdt[n].limit_lo = limit & 0xFFFF;
	gdt[n].gran     = ((limit >> 16) & 0x0F) | (gran & 0xF0);
	gdt[n].access   = access;
}

void tss_set_kernel_stack(unsigned int esp0)
{
	tss.esp0 = esp0;
}

unsigned int gdt_boot_kstack_top(void)
{
	return (unsigned int)(kernel_stack + sizeof(kernel_stack));
}

void gdt_init(void)
{
	set_gate(0, 0, 0, 0, 0);                       /* null */
	set_gate(1, 0, 0xFFFFF, 0x9A, 0xCF);          /* kernel code (ring 0) */
	set_gate(2, 0, 0xFFFFF, 0x92, 0xCF);          /* kernel data (ring 0) */
	set_gate(3, 0, 0xFFFFF, 0xFA, 0xCF);          /* user code   (ring 3) */
	set_gate(4, 0, 0xFFFFF, 0xF2, 0xCF);          /* user data   (ring 3) */
	set_gate(5, (unsigned int)&tss, sizeof(tss) - 1, 0x89, 0x00);  /* TSS */

	tss.ss0 = 0x10;                               /* kernel data selector */
	tss.esp0 = (unsigned int)(kernel_stack + sizeof(kernel_stack));
	tss.iomap_base = sizeof(tss);                 /* no I/O permission map */

	gp.limit = sizeof(gdt) - 1;
	gp.base  = (unsigned int)&gdt;

	gdt_flush((unsigned int)&gp);
	tss_flush();
}
