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

/* W^X: re-stamp user PTEs in [va, va+len) with the bits implied by an
 * ELF PT_LOAD's p_flags (PF_X = exec, PF_W = write). Called by the ELF
 * loader after each segment's bytes are in place so .text becomes R-X,
 * .rodata becomes R-NX and .data stays RW-NX. The frame mapping is
 * preserved -- only permission bits change. */
void paging_user_set_segment_perms(unsigned int va, unsigned int len,
                                   int exec, int write);

/* Map the physical LFB at the fixed user virtual address USER_FB_BASE using a
 * 4 MiB PSE PDE with PG_USER. Returns the virtual address the app should use.
 * The mapping is torn down in paging_reset_user_heap so each new app must call
 * SYS_MAP_FB explicitly. */
void         paging_map_fb(unsigned int phys_base);
unsigned int paging_user_fb_base(void);

/* TODO §1 multitasking, step 1: per-process page directories.
 *
 * paging_create_user_pgdir allocates a fresh 4 KiB page directory, copies
 * the running kernel's PDEs into it (so a trap that lands here finds the
 * same kernel code/data at the same addresses), and leaves the user
 * window (USER_BASE..USER_END) empty. Returns the pgdir's physical
 * address (also valid as a virtual pointer because we identity-map all
 * of physical RAM), or NULL on PMM exhaustion.
 *
 * paging_destroy_user_pgdir walks the user PDEs of the given pgdir,
 * frees every present 4 KiB user frame, frees the per-PDE page table,
 * then frees the pgdir page itself. Kernel PDEs are left alone -- they
 * are shared across every pgdir and freeing them would crash the
 * remaining processes.
 *
 * Step 1 deliberately wires no call sites; step 2 will use them via
 * paging_switch from the scheduler. */
unsigned int *paging_create_user_pgdir(void);
void          paging_destroy_user_pgdir(unsigned int *pgdir);

/* TODO §1 multitasking, step 2: load a process's page directory.
 *
 * Called from the scheduler immediately before context_switch so the
 * incoming process resumes with its own address space active. When the
 * incoming pgdir is NULL/0 (a legacy proc that still shares the boot
 * page_dir) we do nothing -- keeps the single-process boot path
 * bit-identical until step 3 starts handing out per-proc pgdirs. */
void paging_switch(unsigned int *pgdir);

/* TODO §1 multitasking, step 3: build a user address space into a fresh pgdir.
 *
 * paging_init_user_pgdir allocates one 4 KiB page table per user PDE,
 * maps every code page and every stack page (same layout as the legacy
 * paging_init_user), and resets the framebuffer PDE to its kernel
 * identity mapping (each app must SYS_MAP_FB again). The pgdir argument
 * must come from paging_create_user_pgdir. Returns 0 on success, -1 on
 * PMM exhaustion -- partially-populated pgdirs should be cleaned up
 * with paging_destroy_user_pgdir.
 *
 * paging_activate is the in-kernel half of paging_switch: it does the
 * CR3 load AND refreshes the kernel's cache of "currently active user
 * page tables + heap watermark + FB-mapped flag" so subsequent
 * map_user_page / paging_user_alloc / paging_map_fb calls target the
 * new pgdir. Use it from sys_exec after the new pgdir is populated. */
int  paging_init_user_pgdir(unsigned int *pgdir);
void paging_activate(unsigned int *pgdir);

/* Returns the pgdir the kernel created at boot via paging_init. Used by
 * sys_exec on the first call (when current->cr3 is still 0) so we know
 * which pgdir to free as the "old image". */
unsigned int *paging_boot_pgdir(void);

/* TODO §1 multitasking, step 4: deep-copy a parent pgdir's user pages
 * into a child pgdir for sys_fork.
 *
 * Walks every USER PDE of `src`; for each present PT entry, allocates a
 * fresh physical frame, copies 4 KiB from the parent's frame into the
 * child's, and installs an equivalently-flagged PTE in `dst`. The PDE
 * flags (PG_PRESENT | PG_RW | PG_USER) are preserved. PSE entries (the
 * FB) are not copied -- paging_create_user_pgdir already inherited
 * those from the running kernel pgdir, which is correct (FB is shared
 * physical MMIO).
 *
 * `dst` must come from paging_create_user_pgdir and be otherwise empty
 * in its USER range. Returns 0 on success, -1 on PMM exhaustion (the
 * caller should free `dst` via paging_destroy_user_pgdir). No COW. */
int paging_copy_user_pgdir(unsigned int *dst, unsigned int *src);

#endif
