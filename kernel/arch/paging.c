/* Paging. The kernel address space is a flat identity map built from 4 MiB
 * pages (PSE). The userland region spans multiple 4 MiB PDEs, each backed by
 * its own 4 KiB page table so the app's code, stack and heap can be mapped at
 * page granularity. Only those user pages carry PG_USER, so every other PDE
 * stays supervisor and a ring-3 access outside the app's region faults. The
 * code and stack are mapped upfront when the kernel boots; heap pages are
 * allocated on demand (an app that never touches the heap costs no frames). */
#include "paging.h"
#include "pmm.h"

#define PG_PRESENT 0x001
#define PG_RW      0x002
#define PG_USER    0x004
#define PG_PS      0x080          /* 4 MiB page */

#define PAGE_4MB    0x400000u
#define PAGE_4KB    0x1000u
#define ENTRIES     1024
#define FRAME_MASK  0xFFFFF000u

/* User region layout. The USER_PDES contiguous 4 MiB PDEs starting at
 * USER_BASE form a 16 MiB virtual range, carved into:
 *   CODE+DATA+BSS : [USER_CODE,   USER_CODE_END)   1 MiB  (256 pages)
 *   STACK         : [USER_STACK_LO, USER_STACK_TOP) 256 KiB (64 pages)
 *   HEAP          : [USER_HEAP_LO, USER_HEAP_HI)   ~14 MiB (grows lazily)
 * The stack grows down from USER_STACK_TOP; the heap grows up from
 * USER_HEAP_LO via paging_user_alloc. */
#define USER_BASE        0x400000u
#define USER_PDES        4u
#define USER_END         (USER_BASE + USER_PDES * PAGE_4MB)

#define USER_CODE        USER_BASE
#define USER_CODE_PAGES  256u
#define USER_CODE_END    (USER_CODE + USER_CODE_PAGES * PAGE_4KB)

#define USER_STACK_PAGES 64u
#define USER_STACK_LO    USER_CODE_END
#define USER_STACK_TOP   (USER_STACK_LO + USER_STACK_PAGES * PAGE_4KB)

#define USER_HEAP_LO     USER_STACK_TOP
#define USER_HEAP_HI     USER_END

/* Userland framebuffer window: the physical LFB is mapped read/write into this
 * high, kernel-unused virtual range so a ring-3 app can blit straight to video
 * memory (double-buffering, no syscall per rectangle). Spans up to
 * USER_FB_PDES * 4 MiB of LFB, well clear of the pmm-managed RAM (1..32 MiB). */
#define USER_FB_BASE     0x10000000u
#define USER_FB_PDES     4u

static unsigned int *page_dir;
static unsigned int *user_pts[USER_PDES];        /* one 4 KiB PT per user PDE */
static unsigned int  user_heap_next;             /* next unmapped heap address */
static int           fb_pde_mapped;              /* 1 while the user FB PDE is live */

static void load_cr3(void)
{
	__asm__ volatile ("mov %0, %%cr3" : : "r"((unsigned int)page_dir) : "memory");
}

void paging_init(void)
{
	page_dir = (unsigned int *)pmm_alloc();

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

/* Locate the PTE backing 'vaddr' within the user range. Returns 0 outside it. */
static unsigned int *user_pte(unsigned int vaddr)
{
	if (vaddr < USER_BASE || vaddr >= USER_END)
		return 0;
	unsigned int pde = (vaddr - USER_BASE) / PAGE_4MB;
	return &user_pts[pde][(vaddr >> 12) & 0x3FF];
}

static void map_user_page(unsigned int vaddr, unsigned int frame)
{
	unsigned int *pte = user_pte(vaddr);
	*pte = (frame & FRAME_MASK) | PG_PRESENT | PG_RW | PG_USER;
}

/* Tear down the mapping at 'vaddr' and release its physical frame back to the
 * PMM. Invalidates the stale TLB entry so the next access fresh-walks. */
static void unmap_user_page(unsigned int vaddr)
{
	unsigned int *pte = user_pte(vaddr);
	if (!pte || !(*pte & PG_PRESENT))
		return;
	unsigned int frame = *pte & FRAME_MASK;
	*pte = 0;
	pmm_free(frame);
	__asm__ volatile ("invlpg (%0)" : : "r"(vaddr) : "memory");
}

/* Lay out the user address space into the given pgdir: one 4 KiB page
 * table per user PDE plus code+stack pages mapped at their fixed VAs.
 * The heap stays empty (paging_user_alloc grows it on demand). Returns
 * 0 on success, -1 on PMM exhaustion -- caller should clean up via
 * paging_destroy_user_pgdir.
 *
 * Refs:
 *   - PRIMARY xv6-public/vm.c:182 (inituvm) + vm.c:204 (loaduvm): split
 *       between "set up the empty pages" and "copy the ELF in". We
 *       fold them together because our caller (sys_exec) always does
 *       both -- inituvm-only is xv6's exec preamble, not a separate
 *       use case for us.
 *   - CONTRAST linux-0.01/mm/memory.c get_free_page: same pattern of
 *       "alloc-and-zero-PT, hook into PD with present+rw+user", just
 *       wrapped in their per-task LDT model.
 */
int paging_init_user_pgdir(unsigned int *pgdir)
{
	for (unsigned int p = 0; p < USER_PDES; p++) {
		unsigned int *pt = (unsigned int *)pmm_alloc();
		if (!pt)
			return -1;
		for (unsigned int i = 0; i < ENTRIES; i++)
			pt[i] = 0;
		pgdir[(USER_BASE / PAGE_4MB) + p] =
			(unsigned int)pt | PG_PRESENT | PG_RW | PG_USER;
	}
	for (unsigned int i = 0; i < USER_CODE_PAGES; i++) {
		unsigned int frame = pmm_alloc();
		if (!frame) return -1;
		unsigned int va = USER_CODE + i * PAGE_4KB;
		unsigned int *pt = (unsigned int *)(pgdir[va / PAGE_4MB] & FRAME_MASK);
		pt[(va >> 12) & 0x3FF] = (frame & FRAME_MASK) | PG_PRESENT | PG_RW | PG_USER;
	}
	for (unsigned int i = 0; i < USER_STACK_PAGES; i++) {
		unsigned int frame = pmm_alloc();
		if (!frame) return -1;
		unsigned int va = USER_STACK_LO + i * PAGE_4KB;
		unsigned int *pt = (unsigned int *)(pgdir[va / PAGE_4MB] & FRAME_MASK);
		pt[(va >> 12) & 0x3FF] = (frame & FRAME_MASK) | PG_PRESENT | PG_RW | PG_USER;
	}
	/* FB PDE: reset to kernel identity (PSE, no PG_USER). Each app must
	 * call SYS_MAP_FB explicitly to gain ring-3 access to the LFB. */
	unsigned int fb_idx = USER_FB_BASE / PAGE_4MB;
	pgdir[fb_idx] = (fb_idx * PAGE_4MB) | PG_PRESENT | PG_RW | PG_PS;
	return 0;
}

/* Swap CR3 to pgdir and refresh the kernel's caches of "currently
 * active user PTs + heap watermark + FB-mapped flag" so every
 * map_user_page / paging_user_alloc / paging_map_fb call that follows
 * targets the new pgdir. The heap rewinds to USER_HEAP_LO and FB is
 * marked unmapped -- the new image owns its own heap and FB state.
 *
 * Refs:
 *   - PRIMARY xv6-public/vm.c:157 (switchuvm) is the canonical "lcr3
 *       to a per-proc pgdir" entry point; xv6 also retargets the TSS
 *       here, which we do separately in trap.c.
 *   - CONTRAST toaruos/kernel/sys/process.c uses arch_load_page_directory
 *       behind a clone+swap wrapper; semantically identical, more
 *       abstraction layers.
 */
void paging_activate(unsigned int *pgdir)
{
	if (!pgdir)
		return;
	page_dir = pgdir;
	for (unsigned int p = 0; p < USER_PDES; p++) {
		unsigned int pde = pgdir[(USER_BASE / PAGE_4MB) + p];
		user_pts[p] = (pde & PG_PRESENT) ? (unsigned int *)(pde & FRAME_MASK) : 0;
	}
	user_heap_next = USER_HEAP_LO;
	fb_pde_mapped  = 0;
	__asm__ volatile ("mov %0, %%cr3" : : "r"((unsigned int)pgdir) : "memory");
}

unsigned int *paging_boot_pgdir(void)
{
	return page_dir;
}

/* Build the userland address space at boot. Wraps paging_init_user_pgdir
 * around the live page_dir so legacy callers stay unchanged. */
void paging_init_user(void)
{
	paging_init_user_pgdir(page_dir);
	user_heap_next = USER_HEAP_LO;
	load_cr3();
}

unsigned int paging_user_code(void)       { return USER_CODE; }
unsigned int paging_user_code_end(void)   { return USER_CODE_END; }
unsigned int paging_user_stack_top(void)  { return USER_STACK_TOP; }

/* Release every heap page the previous app mapped, then rewind the bump
 * pointer. Also restores the FB PDE to a kernel-only identity mapping so the
 * next app cannot access the LFB until it explicitly calls SYS_MAP_FB.
 * Counts and logs the frame reclamation so a memory-leak sweep can spot
 * regressions in app teardown. */
void paging_reset_user_heap(void)
{
	unsigned int reclaimed = 0;
	for (unsigned int v = USER_HEAP_LO; v < user_heap_next; v += PAGE_4KB) {
		unmap_user_page(v);
		reclaimed++;
	}
	user_heap_next = USER_HEAP_LO;

	if (fb_pde_mapped) {
		unsigned int idx = USER_FB_BASE / PAGE_4MB;
		page_dir[idx] = (idx * PAGE_4MB) | PG_PRESENT | PG_RW | PG_PS;
		fb_pde_mapped = 0;
		load_cr3();
	}

	if (reclaimed > 0) {
		extern void serial_write(const char *);
		extern void serial_write_dec(unsigned int);
		extern void serial_putc(char);
		serial_write("paging: reclaimed ");
		serial_write_dec(reclaimed);
		serial_write(" user heap frames\n");
	}
}

/* Map the physical LFB at USER_FB_BASE using a single 4 MiB PSE page so
 * ring-3 apps can write directly to VRAM (zero syscalls per pixel). The
 * physical address must be 4 MiB-aligned; VBE guarantees this in practice.
 * Calling this again overwrites any prior mapping (safe for one LFB). */
void paging_map_fb(unsigned int phys_base)
{
	if (phys_base == 0)
		return;
	unsigned int idx  = USER_FB_BASE / PAGE_4MB;
	unsigned int aligned = phys_base & ~(PAGE_4MB - 1u);
	page_dir[idx] = aligned | PG_PRESENT | PG_RW | PG_USER | PG_PS;
	fb_pde_mapped = 1;
	load_cr3();
}

unsigned int paging_user_fb_base(void) { return USER_FB_BASE; }

/* Lazily map and return the next 4 KiB heap page (a virtual address), or 0 if
 * the heap is full or physical memory is exhausted. */
unsigned int paging_user_alloc(void)
{
	if (user_heap_next >= USER_HEAP_HI)
		return 0;
	unsigned int frame = pmm_alloc();
	if (frame == 0)
		return 0;
	unsigned int v = user_heap_next;
	map_user_page(v, frame);
	user_heap_next += PAGE_4KB;
	return v;
}

unsigned int paging_user_brk(void)
{
	return user_heap_next;
}

unsigned int paging_user_mmap(unsigned int size)
{
	if (size == 0)
		return 0;
	/* Overflow guard: a hostile userland passing size ~= 0xFFFFFFFF
	 * could otherwise wrap the (size + PAGE_4KB - 1) round-up to a
	 * tiny page count, sneak past the heap-top check, and pull the
	 * allocator into a million-page loop. Cap at the remaining heap
	 * room (rounded down to page granularity). */
	unsigned int room = USER_HEAP_HI - user_heap_next;
	if (size > room)
		return 0;
	unsigned int pages = (size + PAGE_4KB - 1u) / PAGE_4KB;
	if (user_heap_next + pages * PAGE_4KB > USER_HEAP_HI)
		return 0;

	unsigned int base = user_heap_next;
	for (unsigned int i = 0; i < pages; i++) {
		unsigned int frame = pmm_alloc();
		if (!frame) {
			/* Unwind every page we managed to allocate so the user
			 * heap doesn't end up with half-mapped reservations. */
			for (unsigned int j = 0; j < i; j++)
				unmap_user_page(base + j * PAGE_4KB);
			user_heap_next = base;
			return 0;
		}
		map_user_page(user_heap_next, frame);
		user_heap_next += PAGE_4KB;
	}
	return base;
}

unsigned int paging_user_mapped_pages(void)
{
	unsigned int n = 0;
	for (unsigned int p = 0; p < USER_PDES; p++) {
		unsigned int *pt = user_pts[p];
		if (!pt)
			continue;
		for (unsigned int e = 0; e < ENTRIES; e++)
			if (pt[e] & PG_PRESENT)
				n++;
	}
	return n;
}

int paging_user_munmap(unsigned int addr, unsigned int size)
{
	if (size == 0)
		return 0;
	if (addr < USER_HEAP_LO || addr >= USER_HEAP_HI)
		return -1;
	/* Same overflow guard as paging_user_mmap: clamp size to the
	 * remaining heap so the (size + PAGE_4KB - 1) round-up never
	 * wraps a hostile request to a tiny page count. */
	unsigned int room = USER_HEAP_HI - addr;
	if (size > room)
		return -1;
	unsigned int pages = (size + PAGE_4KB - 1u) / PAGE_4KB;
	for (unsigned int i = 0; i < pages; i++) {
		unsigned int v = addr + i * PAGE_4KB;
		if (v >= USER_HEAP_HI)
			return -1;
		unmap_user_page(v);
	}
	return 0;
}

/* Resize the heap so the program break sits at (or just above) 'new_brk',
 * mapping or unmapping pages as needed. Sub-page requests are rounded up so
 * the break always sits on a 4 KiB boundary. Returns the resulting break --
 * equal to the requested page-aligned target on success, or a smaller value
 * if growth ran out of physical memory or hit USER_HEAP_HI. */
unsigned int paging_user_set_brk(unsigned int new_brk)
{
	if (new_brk < USER_HEAP_LO)
		new_brk = USER_HEAP_LO;
	if (new_brk > USER_HEAP_HI)
		new_brk = USER_HEAP_HI;

	unsigned int aligned = (new_brk + PAGE_4KB - 1) & ~(PAGE_4KB - 1);

	while (user_heap_next < aligned) {
		unsigned int frame = pmm_alloc();
		if (frame == 0)
			return user_heap_next;          /* OOM: partial growth */
		map_user_page(user_heap_next, frame);
		user_heap_next += PAGE_4KB;
	}
	while (user_heap_next > aligned) {
		user_heap_next -= PAGE_4KB;
		unmap_user_page(user_heap_next);
	}
	return user_heap_next;
}

/* ---- TODO §1 multitasking, step 1: per-process page directories ----
 *
 * These two routines are the foundation everything else in §1 stacks
 * onto. They build / tear down a self-contained per-process address
 * space without touching any of the legacy globals above (page_dir,
 * user_pts[], user_heap_next, fb_pde_mapped) -- step 1 deliberately
 * adds no call sites. Steps 2-4 rewire the rest of paging.c to operate
 * against a passed-in pgdir, at which point the globals retire.
 *
 * Refs picked for this commit:
 *   - PRIMARY: osdev-refs/xv6-public/vm.c
 *       setupkvm  (line 119): allocate PD, map the kernel into it via a
 *                              kmap[] table. We diverge -- xv6 has no
 *                              4 MiB PSE pages; it walks kmap[] and calls
 *                              mappages for each region. We don't need
 *                              that machinery because our kernel mapping
 *                              is just "PSE identity-map of all 4 GiB"
 *                              that already lives in the running pgdir,
 *                              so a single memcpy is exactly equivalent.
 *       freevm   (line 284): walks PDE -> PTE -> kfree each frame, then
 *                              kfree(pgdir). Same loop, our 4 MiB user
 *                              PDE range = (USER_BASE / PAGE_4MB ..
 *                              USER_END / PAGE_4MB).
 *   - CONTRAST: osdev-refs/linux-0.01/mm/memory.c
 *       free_page_tables (line 79): Linus's version. Loops over PDEs,
 *                              releases each PT page, decrements a
 *                              refcount in mem_map (for COW). We do NOT
 *                              do refcounts because no COW in v1 -- each
 *                              user page is owned by exactly one pgdir,
 *                              freed once. Simpler, but bears watching
 *                              when COW lands later.
 *
 * Why memcpy the kernel half instead of mappages: our paging_init laid
 * out 1024 4 MiB PSE PDEs covering 0..4 GiB; that mapping IS the kernel
 * address space. Cloning it byte-for-byte preserves the same kernel
 * code/data view through any later trap on the new pgdir. Cost: 4 KiB
 * memcpy per process create.
 */
unsigned int *paging_create_user_pgdir(void)
{
	unsigned int *pgdir = (unsigned int *)pmm_alloc();
	if (!pgdir)
		return 0;
	/* Inherit the running kernel mapping (every PSE PDE plus whatever
	 * is in the user-window slots right now) then wipe the user
	 * window so the new process starts with no mappings of its own. */
	for (unsigned int i = 0; i < ENTRIES; i++)
		pgdir[i] = page_dir[i];
	unsigned int user_first = USER_BASE / PAGE_4MB;
	unsigned int user_last  = USER_END  / PAGE_4MB;
	for (unsigned int i = user_first; i < user_last; i++)
		pgdir[i] = 0;
	return pgdir;
}

/* Refs:
 *   - PRIMARY xv6-public/vm.c:157 (switchuvm): xv6 wraps the lcr3 in
 *       pushcli()/popcli() because its scheduler may run with interrupts
 *       enabled. We don't need to do that here -- paging_switch is
 *       called from proc_yield, which is itself only invoked from
 *       irq_handler (interrupts already off via the asm IRQ stub).
 *       Callers from cooperative contexts must hold their own IF=0.
 *   - CONTRAST toaruos/kernel/sys/process.c (switch_next): uses an
 *       arch_set_kernel_stack + arch_load_page_directory pair; same
 *       split as xv6 but with C++-ish naming. Our PCB carries cr3
 *       directly so we don't need the indirection.
 *
 * cr3 = 0 means "this proc still shares the boot page_dir" -- skip the
 * MOV CR3 to keep the single-process boot path identical until step 3
 * starts handing out per-proc pgdirs. */
void paging_switch(unsigned int *pgdir)
{
	if (!pgdir)
		return;
	__asm__ volatile ("mov %0, %%cr3" : : "r"((unsigned int)pgdir) : "memory");
}

void paging_destroy_user_pgdir(unsigned int *pgdir)
{
	if (!pgdir)
		return;
	unsigned int user_first = USER_BASE / PAGE_4MB;
	unsigned int user_last  = USER_END  / PAGE_4MB;
	for (unsigned int i = user_first; i < user_last; i++) {
		unsigned int pde = pgdir[i];
		if (!(pde & PG_PRESENT))
			continue;
		if (pde & PG_PS) {
			/* 4 MiB user PSE page (e.g. the FB PDE if the user mapped it).
			 * Skip -- the FB is host MMIO, not pmm-managed RAM. */
			continue;
		}
		unsigned int *pt = (unsigned int *)(pde & FRAME_MASK);
		for (unsigned int e = 0; e < ENTRIES; e++) {
			unsigned int pte = pt[e];
			if (pte & PG_PRESENT)
				pmm_free(pte & FRAME_MASK);
		}
		pmm_free((unsigned int)pt);
	}
	pmm_free((unsigned int)pgdir);
}

/* Validate a userland buffer before the kernel dereferences it: the range must
 * sit wholly inside one mapped span -- code+stack (contiguous) or the portion
 * of the heap that has actually been mapped so far. */
int paging_user_range_ok(unsigned int addr, unsigned int len)
{
	if (len == 0)
		return 1;
	if (addr + len < addr)                              /* address overflow */
		return 0;

	unsigned int end = addr + len;
	if (addr >= USER_CODE && end <= USER_STACK_TOP)
		return 1;
	if (addr >= USER_HEAP_LO && end <= user_heap_next)
		return 1;
	return 0;
}
