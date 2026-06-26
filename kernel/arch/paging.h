/* Paging: identity-map the kernel and back the userland region with 4 KiB pages. */
#ifndef PAGING_H
#define PAGING_H

void paging_init(void);            /* identity-map 4 GiB (4 MiB pages), enable PG */
void paging_init_user(void);       /* build the userland 4 KiB address space */

unsigned int paging_user_code(void);       /* app load/run virtual address */
unsigned int paging_user_code_end(void);   /* end of the mapped code region */
unsigned int paging_user_stack_top(void);  /* top of the user stack */
unsigned int paging_user_alloc(void);      /* next pre-mapped user heap page, or 0 */
unsigned int paging_user_brk(void);        /* current program break (heap end) */
unsigned int paging_user_set_brk(unsigned int new_brk);  /* resize, returns new break */
void paging_reset_user_heap(void);         /* reset the heap for a new app */

/* True if [addr, addr+len) lies entirely within the app's mapped user pages. */
int paging_user_range_ok(unsigned int addr, unsigned int len);

/* Map the physical LFB at the fixed user virtual address USER_FB_BASE using a
 * 4 MiB PSE PDE with PG_USER. Returns the virtual address the app should use.
 * The mapping is torn down in paging_reset_user_heap so each new app must call
 * SYS_MAP_FB explicitly. */
void         paging_map_fb(unsigned int phys_base);
unsigned int paging_user_fb_base(void);

#endif
