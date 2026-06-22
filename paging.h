/* Paging: identity-map the kernel and back the userland region with 4 KiB pages. */
#ifndef PAGING_H
#define PAGING_H

void paging_init(void);            /* identity-map 4 GiB (4 MiB pages), enable PG */
void paging_init_user(void);       /* build the userland 4 KiB address space */

unsigned int paging_user_code(void);       /* app load/run virtual address */
unsigned int paging_user_stack_top(void);  /* top of the user stack */
unsigned int paging_user_alloc(void);      /* next pre-mapped user heap page, or 0 */
void paging_reset_user_heap(void);         /* reset the heap for a new app */

#endif
