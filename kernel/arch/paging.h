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

/* Reserve a contiguous block of `size` bytes (rounded up to 4 KiB pages) in
 * the user heap region and back it with fresh physical frames. Returns the
 * base virtual address, or 0 if the heap is full or out of memory. Unlike
 * sbrk these allocations do not contribute to the program break -- they sit
 * past it -- so malloc/free remain untouched. */
unsigned int paging_user_mmap(unsigned int size);

/* Release the pages previously returned by paging_user_mmap. The freed range
 * stays reserved in the heap bump pointer (no compaction), so the address
 * cannot be reused until the next paging_reset_user_heap. Returns 0 on
 * success, -1 if the range falls outside the heap region. */
int          paging_user_munmap(unsigned int addr, unsigned int size);

/* Diagnostics: counts present PTEs across the user region. The kernel logs
 * a "paging: reclaimed N frames" line on every app reset; a follow-up call
 * lets a test harness assert "the user region is fully drained" instead of
 * trusting the log. Excludes the framebuffer PSE PDE -- it is mapped on
 * demand by SYS_MAP_FB and tracked separately. */
unsigned int paging_user_mapped_pages(void);

/* True if [addr, addr+len) lies entirely within the app's mapped user pages. */
int paging_user_range_ok(unsigned int addr, unsigned int len);

/* Map the physical LFB at the fixed user virtual address USER_FB_BASE using a
 * 4 MiB PSE PDE with PG_USER. Returns the virtual address the app should use.
 * The mapping is torn down in paging_reset_user_heap so each new app must call
 * SYS_MAP_FB explicitly. */
void         paging_map_fb(unsigned int phys_base);
unsigned int paging_user_fb_base(void);

#endif
