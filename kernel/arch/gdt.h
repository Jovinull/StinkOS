/* Global Descriptor Table with kernel + user segments and a TSS. */
#ifndef GDT_H
#define GDT_H

void gdt_init(void);                       /* build the GDT + TSS, lgdt, ltr */
void tss_set_kernel_stack(unsigned int esp0);  /* ring0 stack for ring3 traps */

/* Top (exclusive) of the kernel_stack[] BSS array gdt_init points TSS.esp0
 * at by default. Used by proc_init to record PID 1's kstack_top so that
 * paging_switch can update TSS.esp0 to PID 1's stack the same way it does
 * for forked children. Without this, both procs use the same kstack and
 * the SECOND-to-enter-kernel one overwrites the FIRST's saved frame. */
unsigned int gdt_boot_kstack_top(void);

#endif
