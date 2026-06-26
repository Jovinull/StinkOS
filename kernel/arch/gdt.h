/* Global Descriptor Table with kernel + user segments and a TSS. */
#ifndef GDT_H
#define GDT_H

void gdt_init(void);                       /* build the GDT + TSS, lgdt, ltr */
void tss_set_kernel_stack(unsigned int esp0);  /* ring0 stack for ring3 traps */

#endif
