/* Paging. v0.5 PAE 3-level walker: CR3 -> PDPT (4 entries) -> PD (512
 * entries x 8 bytes; PSE PDEs map 2 MiB huge pages, non-PSE PDEs point
 * at a 4 KiB PT) -> PT (512 entries x 8 bytes). Higher-half kernel
 * still at virt 0x80100000 (linked unchanged); the PAE switch only
 * widens the walker so PTE bit 63 (NX) becomes available for §7 W^X.
 *
 * Per-process address space layout:
 *   PDPT[0] -> per-proc PD0  -- user [0x400000, 0x1400000)
 *   PDPT[1] -> shared kernel_pd1  -- virt [1 GiB, 2 GiB) (empty today)
 *   PDPT[2] -> shared kernel_pd2  -- virt [KERNBASE, KERNBASE+1 GiB)
 *                                    direct map of phys RAM (PSE 2 MiB)
 *   PDPT[3] -> shared kernel_pd3  -- virt [3 GiB, 4 GiB), DEVSPACE MMIO
 *
 * Sharing PD1/PD2/PD3 across every proc means kernel mapping changes
 * are picked up automatically without per-pgdir touch; only PD0
 * differs per process. Same shape as xv6-public's setupkvm sharing
 * kmap[] across procs, just split across PDPT slots because PAE
 * forces a 3-level walk.
 *
 * Refs:
 *   - Intel SDM Vol 3A §4.4 (PAE Paging) figure 4-9 (PDE/PTE format
 *     with 8-byte entries + NX at bit 63)
 *   - xv6-riscv kernel/vm.c walk() -- canonical multi-level walk
 *     pattern (Sv39 = 3-level analog)
 *   - serenity Kernel/Arch/x86_64/PageDirectory.h -- 8-byte PTE
 *     accessor + NoExecute bit (0x8000000000000000) shape
 */
#include "paging.h"
#include "pmm.h"
#include "memlayout.h"
#include "cpuid.h"
#include "serial.h"

/* NX bit lives at bit 63 of an 8-byte PTE/PDE (Intel SDM Vol 3A §4.6.2).
 * On 32-bit literals: split as two halves so the constant is well-formed
 * in C without 64-bit suffixes -- we shift into place below. */
#define PG_NX_HI 0x80000000u
#define PG_NX    (((pae_entry_t)PG_NX_HI) << 32)

/* Set once at paging_init from CPUID + EFER state. When 0, setting bit 63
 * in a PTE while IA32_EFER.NXE=0 triggers a reserved-bit page fault on
 * access -- so we must skip stamping NX on CPUs that don't advertise it
 * (or where kentry.s skipped the WRMSR). */
static int g_nx_enabled;

#define PAGE_4KB    0x1000u
#define PAGE_2MB    0x200000u
#define PAGE_1GB    0x40000000u

#define PDPT_ENTRIES 4u
#define PD_ENTRIES   512u
#define PT_ENTRIES   512u

#define PDPT_IDX(va) (((va) >> 30) & 0x3u)
#define PD_IDX(va)   (((va) >> 21) & 0x1FFu)
#define PT_IDX(va)   (((va) >> 12) & 0x1FFu)

/* Phys-address mask covering bits 12-35 (PAE supports 36-bit phys).
 * The upper 28 bits of the 64-bit entry are reserved/NX; we mask down
 * to what the CPU treats as the frame pointer. */
#define FRAME_MASK_4KB  0x0000000FFFFFF000ULL
#define FRAME_MASK_2MB  0x0000000FFFE00000ULL

/* User region layout (unchanged from v0.4 -- USER_BASE etc stay where
 * apps + linker scripts expect them). USER_PDES_2MB counts 2 MiB PDEs
 * because under PAE the PSE page size halved from 4 MiB. */
#define USER_BASE         0x400000u
#define USER_END          0x1400000u
#define USER_SPAN         (USER_END - USER_BASE)
#define USER_PDES_2MB     (USER_SPAN / PAGE_2MB)        /* 8 PDEs */

#define USER_CODE         USER_BASE
#define USER_CODE_PAGES   256u
#define USER_CODE_END     (USER_CODE + USER_CODE_PAGES * PAGE_4KB)

#define USER_STACK_PAGES  64u
#define USER_STACK_LO     USER_CODE_END
#define USER_STACK_TOP    (USER_STACK_LO + USER_STACK_PAGES * PAGE_4KB)

#define USER_HEAP_LO      USER_STACK_TOP
#define USER_HEAP_HI      USER_END

#define USER_FB_BASE      0x10000000u

typedef unsigned long long pae_entry_t;

/* The PCB->cr3 field stores a KVA pointer to the active PDPT
 * (kernel-only high-half virt). paging_switch() applies V2P before
 * the actual CR3 load so the CPU sees a phys address. Treating it
 * as `unsigned int *` keeps the ABI matching what proc.c / syscall.c
 * already pass around -- only the internal interpretation changed
 * from "PD pointer" to "PDPT pointer". */
static pae_entry_t *page_pdpt;          /* KVA of active PDPT (4 entries used) */
static unsigned int user_heap_next;
static int          fb_pde_mapped;

/* Shared kernel PDs (phys addrs). Built once at boot; every per-proc
 * PDPT points its slots [1..3] at the same physical PDs so all
 * processes see identical kernel mappings without per-create copy. */
static unsigned int kernel_pd1_phys;    /* [1 GiB, 2 GiB) -- empty today */
static unsigned int kernel_pd2_phys;    /* [KERNBASE, +1 GiB) direct map  */
static unsigned int kernel_pd3_phys;    /* [3 GiB, 4 GiB) DEVSPACE MMIO   */

#define KVA(phys) ((pae_entry_t *)P2V((unsigned int)(phys)))
#define KVA32(phys) ((unsigned int *)P2V((unsigned int)(phys)))

static void load_cr3_active(void)
{
	__asm__ volatile ("mov %0, %%cr3" : :
	                  "r"(V2P((unsigned int)page_pdpt)) : "memory");
}

static void zero_frame(unsigned int phys)
{
	pae_entry_t *p = KVA(phys);
	for (unsigned int i = 0; i < PAGE_4KB / sizeof(pae_entry_t); i++)
		p[i] = 0;
}

/* Build a shared kernel PD that PSE-maps a 1 GiB virt window.
 *
 * virt_base: top of the 1 GiB window this PD covers (must be 1 GiB-aligned).
 * phys_base: phys address virt_base maps to; for the high-half direct
 *            map this is 0 (virt KERNBASE -> phys 0); for DEVSPACE
 *            identity this is virt_base itself.
 * present_count: how many 2 MiB PDEs to fill from PD[0..]. The rest
 *                stay zero (not present). For the direct map we cover
 *                only [0, KERNEL_DIRECT_MAP); for DEVSPACE we cover
 *                the full 1 GiB from KERNEL_DEVSPACE to 4 GiB.
 */
static void build_kernel_pd(unsigned int pd_phys,
                            unsigned int phys_base,
                            unsigned int present_count,
                            pae_entry_t extra_flags)
{
	zero_frame(pd_phys);
	pae_entry_t *pd = KVA(pd_phys);
	for (unsigned int i = 0; i < present_count; i++) {
		unsigned int phys = phys_base + i * PAGE_2MB;
		pd[i] = (pae_entry_t)phys | 0x83ULL | extra_flags;
	}
}

static void build_kernel_pds(void)
{
	kernel_pd1_phys = pmm_alloc();
	kernel_pd2_phys = pmm_alloc();
	kernel_pd3_phys = pmm_alloc();
	pae_entry_t nx = g_nx_enabled ? PG_NX : 0;
	/* PD1: virt [1 GiB, 2 GiB) -- nothing kernel-side here, leave empty. */
	build_kernel_pd(kernel_pd1_phys, 0, 0, 0);
	/* PD2: virt [KERNBASE, KERNBASE+1 GiB) -> phys [0, KERNEL_DIRECT_MAP).
	 * Mark the whole direct map NX so the kernel cannot accidentally
	 * jump into ordinary data frames -- only the first 2 MiB PSE PDE
	 * (which kernel_wx_install_pt later splits into a 4 KiB PT) carries
	 * any executable virt for the kernel image. */
	build_kernel_pd(kernel_pd2_phys, 0, KERNEL_DIRECT_MAP / PAGE_2MB, nx);
	/* PD3: virt [3 GiB, 4 GiB) identity from KERNEL_DEVSPACE up. NX on
	 * MMIO BARs is fine -- no driver executes from device memory. */
	{
		zero_frame(kernel_pd3_phys);
		pae_entry_t *pd = KVA(kernel_pd3_phys);
		unsigned int dev_first_idx = (KERNEL_DEVSPACE - 3 * PAGE_1GB) / PAGE_2MB;
		for (unsigned int i = dev_first_idx; i < PD_ENTRIES; i++) {
			unsigned int phys = 3 * PAGE_1GB + i * PAGE_2MB;
			pd[i] = (pae_entry_t)phys | 0x83ULL | nx;
		}
	}
}

/* Linker-script symbols bracketing each kernel section. Page-aligned so
 * the per-page permission walk below never has to special-case partial
 * pages. Resolved at link time -- they are pure virt addresses, no
 * runtime storage. */
extern char __kernel_text_start[];
extern char __kernel_text_end[];
extern char __kernel_rodata_start[];
extern char __kernel_rodata_end[];
extern char __kernel_data_start[];
extern char __kernel_data_end[];

/* Replace PD2[0] (the 2 MiB PSE huge mapping covering virt [KERNBASE,
 * KERNBASE+2 MiB) -> phys [0, 2 MiB)) with a 4 KiB-granularity PT so we
 * can stamp per-page W^X permissions across the kernel image:
 *   .text/.multiboot  R-X       (no W, no NX)
 *   .rodata           R-NX      (no W, NX set)
 *   .data/.bss/stack  RW-NX     (RW, NX set)
 *
 * Every kernel section currently sits inside the first 2 MiB phys window
 * because the image is ~120 KiB and the early PM stack lives at phys
 * [0x80000, 0x90000). One PT (4 KiB) covers the whole window with 512
 * 4 KiB PTEs -- cheap and surgical. Anything outside the kernel image
 * (BIOS data area, unused low phys) gets the conservative RW-NX default
 * so a write-where bug cannot land executable bytes there.
 *
 * Pattern mirrors xv6-public's vm.c per-section R/W setup (it predates
 * NX so it only enforces R vs RW), threaded through PAE for NX support.
 * Refs: Intel SDM Vol 3A §4.6 (page-level protections) + serenity
 *       Kernel/Arch/x86_64/PageDirectory.cpp (kernel image NX split). */
static void kernel_wx_install_pt(void)
{
	unsigned int pt_phys = pmm_alloc();
	zero_frame(pt_phys);
	pae_entry_t *pt = KVA(pt_phys);

	unsigned int text_lo   = (unsigned int)__kernel_text_start;
	unsigned int text_hi   = (unsigned int)__kernel_text_end;
	unsigned int rodata_lo = (unsigned int)__kernel_rodata_start;
	unsigned int rodata_hi = (unsigned int)__kernel_rodata_end;

	pae_entry_t nx = g_nx_enabled ? PG_NX : 0;

	for (unsigned int i = 0; i < PT_ENTRIES; i++) {
		unsigned int virt = KERNBASE + i * PAGE_4KB;
		unsigned int phys = i * PAGE_4KB;
		pae_entry_t  pte  = (pae_entry_t)phys | 1ULL;     /* P */

		if (virt >= text_lo && virt < text_hi) {
			/* R-X: clear RW, clear NX. */
		} else if (virt >= rodata_lo && virt < rodata_hi) {
			/* R-NX: clear RW, set NX. */
			pte |= nx;
		} else {
			/* RW-NX: everything else (data, bss, stack, BIOS scratch). */
			pte |= 2ULL;
			pte |= nx;
		}
		pt[i] = pte;
	}

	pae_entry_t *pd2 = KVA(kernel_pd2_phys);
	pd2[0] = (pae_entry_t)pt_phys | 3ULL;     /* P|RW, no PS -> 4 KiB PT */
}

static void wire_kernel_pdpt(pae_entry_t *pdpt)
{
	pdpt[1] = (pae_entry_t)kernel_pd1_phys | 1ULL;
	pdpt[2] = (pae_entry_t)kernel_pd2_phys | 1ULL;
	pdpt[3] = (pae_entry_t)kernel_pd3_phys | 1ULL;
}

void paging_init(void)
{
	/* kentry's bootstrap PDPT already established identity-low,
	 * high-half mirror and DEVSPACE identity. We now build a runtime
	 * PDPT that:
	 *   - DROPS identity-low (PDPT[0] = 0 until a user proc populates it)
	 *   - SHARES kernel PDs (PD1/PD2/PD3) across every per-proc PDPT.
	 *   - SPLITS PD2[0] into a 4 KiB PT so per-section W^X permissions
	 *     can be stamped on the kernel image (.text RX, .rodata R-NX,
	 *     .data/.bss RW-NX).
	 *
	 * Same xv6 pattern the v0.4 higher-half work introduced, lifted to
	 * PAE with kernel W^X on top. */
	g_nx_enabled = cpuid_has_nx();
	build_kernel_pds();
	kernel_wx_install_pt();

	unsigned int pdpt_phys = pmm_alloc();
	zero_frame(pdpt_phys);
	pae_entry_t *pdpt = KVA(pdpt_phys);
	wire_kernel_pdpt(pdpt);
	pdpt[0] = 0;          /* no user space until paging_init_user */

	page_pdpt = pdpt;
	/* PAE was already enabled by kentry; just swap CR3 to the runtime
	 * PDPT. CR3 reload also flushes the TLB so the kernel image PT
	 * supersedes the bootstrap PSE PDE in time for the next fetch. */
	load_cr3_active();
}

/* Walk the active PDPT and return a pointer (KVA) to the PTE backing
 * `va`. Returns 0 when:
 *   - va isn't in PDPT[0]'s 1 GiB window (we only handle USER there)
 *   - the PD entry is missing or marks a huge 2 MiB page
 * If `alloc` is non-zero and the PT slot is empty, alloc a fresh PT
 * frame, zero it and install. Mirrors xv6-riscv walk() shape. */
static pae_entry_t *walk_user_pte(pae_entry_t *pdpt, unsigned int va, int alloc)
{
	if (PDPT_IDX(va) != 0)
		return 0;
	pae_entry_t pdpte = pdpt[0];
	if (!(pdpte & 1ULL))
		return 0;
	pae_entry_t *pd = KVA((unsigned int)(pdpte & FRAME_MASK_4KB));
	pae_entry_t pde = pd[PD_IDX(va)];
	if (!(pde & 1ULL)) {
		if (!alloc)
			return 0;
		unsigned int pt_phys = pmm_alloc();
		if (!pt_phys)
			return 0;
		zero_frame(pt_phys);
		pde = (pae_entry_t)pt_phys | 0x7ULL;     /* PRESENT|RW|USER */
		pd[PD_IDX(va)] = pde;
	}
	if (pde & 0x80ULL)
		return 0;                                /* huge 2 MiB, no PT */
	pae_entry_t *pt = KVA((unsigned int)(pde & FRAME_MASK_4KB));
	return &pt[PT_IDX(va)];
}

/* Default user PTE: present, RW, user, NX (when CPU supports it). Anything
 * the kernel maps into user space starts non-executable; only an explicit
 * paging_user_set_segment_perms with exec=1 (from the ELF loader on a
 * PF_X segment) clears NX so .text becomes runnable. */
static pae_entry_t user_pte_flags(int exec, int write)
{
	pae_entry_t f = 0x5ULL;                     /* P|U */
	if (write) f |= 0x2ULL;                     /* RW */
	if (!exec && g_nx_enabled) f |= PG_NX;
	return f;
}

static void map_user_page(unsigned int vaddr, unsigned int frame)
{
	pae_entry_t *pte = walk_user_pte(page_pdpt, vaddr, 1);
	if (!pte)
		return;
	*pte = (pae_entry_t)(frame & 0xFFFFF000u) | user_pte_flags(0, 1);
}

/* COW fault resolution. The #PF handler routes a write to a PG_COW
 * page here. Returns 1 when handled, 0 when the PTE has no PG_COW
 * (caller should fall through to the kill path).
 *
 * Two branches:
 *   refcount == 1 -- we're the only owner left; just flip PG_RW back
 *                    on, clear PG_COW. No alloc, no copy.
 *   refcount  > 1 -- alloc a fresh frame, copy the shared one via the
 *                    kernel direct map, install the new frame at this
 *                    PTE with RW set + PG_COW cleared, decrement the
 *                    shared frame's refcount. The other owners keep
 *                    the original frame (RO + PG_COW) until they too
 *                    take the fault.
 *
 * Pattern from toaruos `mmu_copy_on_write` (arch/x86_64/mmu.c:1313). */
int paging_handle_cow_fault(unsigned int va)
{
	pae_entry_t *pte = walk_user_pte(page_pdpt, va, 0);
	if (!pte) return 0;
	pae_entry_t entry = *pte;
	if (!(entry & PG_PRESENT)) return 0;
	if (!(entry & PG_COW)) return 0;
	serial_write("cow: fault va=0x");
	serial_write_hex(va);
	serial_write(" ref=");
	serial_write_dec(pmm_ref((unsigned int)(entry & FRAME_MASK_4KB)));
	serial_putc('\n');

	unsigned int old_frame = (unsigned int)(entry & FRAME_MASK_4KB);
	pae_entry_t  perms_kept = entry & ~((pae_entry_t)PG_COW | FRAME_MASK_4KB);

	if (pmm_ref(old_frame) <= 1u) {
		/* Last owner: just lift the RO + COW marker. The frame
		 * stays the same, refcount stays 1. */
		*pte = (pae_entry_t)old_frame | perms_kept | PG_RW;
	} else {
		unsigned int new_frame = pmm_alloc();
		if (!new_frame) return 0;       /* OOM: let caller kill us */
		unsigned int *sp = KVA32(old_frame);
		unsigned int *dp = KVA32(new_frame);
		for (unsigned int w = 0; w < PAGE_4KB / 4; w++) dp[w] = sp[w];
		*pte = (pae_entry_t)new_frame | perms_kept | PG_RW;
		pmm_free(old_frame);            /* drops shared frame's refcount */
	}
	__asm__ volatile ("invlpg (%0)" : : "r"(va) : "memory");
	return 1;
}

/* Re-stamp the user PTEs covering [va, va+len) with the W^X bits implied
 * by an ELF segment's p_flags (PF_X = exec, PF_W = write). The frame
 * mapping is preserved; only the permission bits change. Called from the
 * ELF loader once per PT_LOAD after the segment bytes are in place. */
void paging_user_set_segment_perms(unsigned int va, unsigned int len,
                                   int exec, int write)
{
	unsigned int aligned_lo = va & ~(PAGE_4KB - 1u);
	unsigned int end        = va + len;
	unsigned int aligned_hi = (end + PAGE_4KB - 1u) & ~(PAGE_4KB - 1u);
	pae_entry_t  perm       = user_pte_flags(exec, write);

	for (unsigned int v = aligned_lo; v < aligned_hi; v += PAGE_4KB) {
		pae_entry_t *pte = walk_user_pte(page_pdpt, v, 0);
		if (!pte || !(*pte & 1ULL))
			continue;
		unsigned int frame = (unsigned int)(*pte & FRAME_MASK_4KB);
		*pte = (pae_entry_t)frame | perm;
		__asm__ volatile ("invlpg (%0)" : : "r"(v) : "memory");
	}
}

static void unmap_user_page(unsigned int vaddr)
{
	pae_entry_t *pte = walk_user_pte(page_pdpt, vaddr, 0);
	if (!pte || !(*pte & 1ULL))
		return;
	unsigned int frame = (unsigned int)(*pte & FRAME_MASK_4KB);
	*pte = 0;
	pmm_free(frame);
	__asm__ volatile ("invlpg (%0)" : : "r"(vaddr) : "memory");
}

/* Lay out a fresh user address space into the given PDPT:
 *   - allocate a PD0 (slot 0 of PDPT)
 *   - allocate one PT per USER PDE (8 PTs covering [USER_BASE, USER_END))
 *   - allocate code + stack frames, install their PTEs
 *   - leave the heap empty (paging_user_alloc grows on demand)
 *
 * `pdpt` is a KVA pointer to the per-proc PDPT (slot 0 expected zero).
 */
int paging_init_user_pgdir(unsigned int *pdpt_raw)
{
	pae_entry_t *pdpt = (pae_entry_t *)pdpt_raw;

	unsigned int pd0_phys = pmm_alloc();
	if (!pd0_phys)
		return -1;
	zero_frame(pd0_phys);
	pdpt[0] = (pae_entry_t)pd0_phys | 1ULL;
	pae_entry_t *pd0 = KVA(pd0_phys);

	/* One 4 KiB PT per USER 2 MiB PDE. PDE itself stays RW+U so the
	 * walker can reach the PT; per-page W^X lives on the PTEs. */
	for (unsigned int p = 0; p < USER_PDES_2MB; p++) {
		unsigned int pt_phys = pmm_alloc();
		if (!pt_phys)
			return -1;
		zero_frame(pt_phys);
		unsigned int pd_idx = PD_IDX(USER_BASE) + p;
		pd0[pd_idx] = (pae_entry_t)pt_phys | 0x7ULL;
	}

	/* Code + stack pages default to RW+U+NX. The ELF loader downgrades
	 * the .text range to R-X via paging_user_set_segment_perms after
	 * writing the segment bytes; stack stays RW+NX forever. */
	pae_entry_t initial = user_pte_flags(0, 1);
	for (unsigned int i = 0; i < USER_CODE_PAGES; i++) {
		unsigned int frame = pmm_alloc();
		if (!frame)
			return -1;
		unsigned int va = USER_CODE + i * PAGE_4KB;
		pae_entry_t *pde_slot = &pd0[PD_IDX(va)];
		pae_entry_t *pt = KVA((unsigned int)(*pde_slot & FRAME_MASK_4KB));
		pt[PT_IDX(va)] = (pae_entry_t)(frame & 0xFFFFF000u) | initial;
	}
	for (unsigned int i = 0; i < USER_STACK_PAGES; i++) {
		unsigned int frame = pmm_alloc();
		if (!frame)
			return -1;
		unsigned int va = USER_STACK_LO + i * PAGE_4KB;
		pae_entry_t *pde_slot = &pd0[PD_IDX(va)];
		pae_entry_t *pt = KVA((unsigned int)(*pde_slot & FRAME_MASK_4KB));
		pt[PT_IDX(va)] = (pae_entry_t)(frame & 0xFFFFF000u) | initial;
	}
	return 0;
}

void paging_activate(unsigned int *pdpt_raw)
{
	if (!pdpt_raw)
		return;
	page_pdpt = (pae_entry_t *)pdpt_raw;
	user_heap_next = USER_HEAP_LO;
	fb_pde_mapped  = 0;
	__asm__ volatile ("mov %0, %%cr3" : :
	                  "r"(V2P((unsigned int)pdpt_raw)) : "memory");
}

unsigned int *paging_boot_pgdir(void)
{
	return (unsigned int *)page_pdpt;
}

/* Copy USER pages from src PDPT into dst PDPT under copy-on-write. No
 * frame contents are duplicated here: every present user PTE has its
 * frame shared with the child via pmm_ref_inc, and writable pages are
 * downgraded RO + tagged PG_COW on BOTH parent and child so the first
 * writer takes a #PF that the COW handler resolves.
 *
 * Read-only pages (PG_RW already clear -- v0.5 W^X .text / .rodata)
 * stay shared without PG_COW; writes to them are real W^X violations
 * and the existing trap path kills the offender.
 *
 * Matches toaruos `copy_page_maybe` (arch/x86_64/mmu.c:442) -- same
 * refcount + per-PTE COW bit design. */
int paging_copy_user_pgdir(unsigned int *dst_raw, unsigned int *src_raw)
{
	pae_entry_t *dst = (pae_entry_t *)dst_raw;
	pae_entry_t *src = (pae_entry_t *)src_raw;

	if (!(dst[0] & 1ULL)) {
		unsigned int pd0_phys = pmm_alloc();
		if (!pd0_phys)
			return -1;
		zero_frame(pd0_phys);
		dst[0] = (pae_entry_t)pd0_phys | 1ULL;
	}
	pae_entry_t *dst_pd0 = KVA((unsigned int)(dst[0] & FRAME_MASK_4KB));
	pae_entry_t *src_pd0 = KVA((unsigned int)(src[0] & FRAME_MASK_4KB));

	unsigned int first = PD_IDX(USER_BASE);
	unsigned int last  = first + USER_PDES_2MB;

	for (unsigned int p = first; p < last; p++) {
		pae_entry_t spde = src_pd0[p];
		if (!(spde & 1ULL))
			continue;
		if (spde & 0x80ULL)
			continue;                            /* 2 MiB huge -- skip (FB MMIO) */

		pae_entry_t *src_pt = KVA((unsigned int)(spde & FRAME_MASK_4KB));
		unsigned int dst_pt_phys = pmm_alloc();
		if (!dst_pt_phys)
			return -1;
		zero_frame(dst_pt_phys);
		pae_entry_t *dst_pt = KVA(dst_pt_phys);
		dst_pd0[p] = (pae_entry_t)dst_pt_phys | (spde & 0xFFFULL);

		for (unsigned int e = 0; e < PT_ENTRIES; e++) {
			pae_entry_t spte = src_pt[e];
			if (!(spte & 1ULL))
				continue;
			unsigned int src_frame =
			    (unsigned int)(spte & FRAME_MASK_4KB);
			unsigned int va =
			    (p << 21) | (e << 12);
			pae_entry_t shared = spte;
			if (spte & PG_RW) {
				/* Writable: strip RW, tag COW on BOTH so a write
				 * from either side faults to the COW handler. */
				shared &= ~((pae_entry_t)PG_RW);
				shared |= PG_COW;
				src_pt[e] = shared;
				__asm__ volatile ("invlpg (%0)"
				                  : : "r"(va) : "memory");
			}
			/* Read-only pages (text/rodata under v0.5 W^X) get
			 * shared as-is: same frame, same perms, no PG_COW.
			 * Any future write traps as a real W^X violation. */
			dst_pt[e] = shared;
			pmm_ref_inc(src_frame);
		}
	}
	return 0;
}

void paging_init_user(void)
{
	paging_init_user_pgdir((unsigned int *)page_pdpt);
	user_heap_next = USER_HEAP_LO;
	load_cr3_active();
}

unsigned int paging_user_code(void)      { return USER_CODE; }
unsigned int paging_user_code_end(void)  { return USER_CODE_END; }
unsigned int paging_user_stack_top(void) { return USER_STACK_TOP; }

void paging_reset_user_heap(void)
{
	unsigned int reclaimed = 0;
	for (unsigned int v = USER_HEAP_LO; v < user_heap_next; v += PAGE_4KB) {
		unmap_user_page(v);
		reclaimed++;
	}
	user_heap_next = USER_HEAP_LO;

	if (fb_pde_mapped) {
		/* Tear down the FB 2 MiB huge PDE we installed in paging_map_fb.
		 * Active PD0 lives in PDPT[0]. */
		pae_entry_t pdpte = page_pdpt[0];
		if (pdpte & 1ULL) {
			pae_entry_t *pd0 =
			    KVA((unsigned int)(pdpte & FRAME_MASK_4KB));
			pd0[PD_IDX(USER_FB_BASE)] = 0;
		}
		fb_pde_mapped = 0;
		load_cr3_active();
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

/* Map the physical LFB at USER_FB_BASE using a 2 MiB PSE huge PDE in
 * the active proc's PD0. The LFB phys is 2 MiB-aligned in QEMU/VBE. */
void paging_map_fb(unsigned int phys_base)
{
	if (phys_base == 0)
		return;
	pae_entry_t pdpte = page_pdpt[0];
	if (!(pdpte & 1ULL))
		return;
	pae_entry_t *pd0 = KVA((unsigned int)(pdpte & FRAME_MASK_4KB));
	unsigned int aligned = phys_base & ~(PAGE_2MB - 1u);
	pd0[PD_IDX(USER_FB_BASE)] = (pae_entry_t)aligned | 0x87ULL; /* P|RW|U|PS */
	fb_pde_mapped = 1;
	load_cr3_active();
}

unsigned int paging_user_fb_base(void) { return USER_FB_BASE; }

unsigned int paging_user_alloc(void)
{
	if (user_heap_next >= USER_HEAP_HI)
		return 0;
	unsigned int frame = pmm_alloc();
	if (!frame)
		return 0;
	unsigned int v = user_heap_next;
	map_user_page(v, frame);
	user_heap_next += PAGE_4KB;
	return v;
}

unsigned int paging_user_brk(void) { return user_heap_next; }

unsigned int paging_user_mmap(unsigned int size)
{
	if (size == 0)
		return 0;
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
	pae_entry_t pdpte = page_pdpt[0];
	if (!(pdpte & 1ULL))
		return 0;
	pae_entry_t *pd0 = KVA((unsigned int)(pdpte & FRAME_MASK_4KB));
	unsigned int first = PD_IDX(USER_BASE);
	for (unsigned int p = 0; p < USER_PDES_2MB; p++) {
		pae_entry_t pde = pd0[first + p];
		if (!(pde & 1ULL) || (pde & 0x80ULL))
			continue;
		pae_entry_t *pt = KVA((unsigned int)(pde & FRAME_MASK_4KB));
		for (unsigned int e = 0; e < PT_ENTRIES; e++)
			if (pt[e] & 1ULL)
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

unsigned int paging_user_set_brk(unsigned int new_brk)
{
	if (new_brk < USER_HEAP_LO)
		new_brk = USER_HEAP_LO;
	if (new_brk > USER_HEAP_HI)
		new_brk = USER_HEAP_HI;

	unsigned int aligned = (new_brk + PAGE_4KB - 1) & ~(PAGE_4KB - 1);

	while (user_heap_next < aligned) {
		unsigned int frame = pmm_alloc();
		if (!frame)
			return user_heap_next;
		map_user_page(user_heap_next, frame);
		user_heap_next += PAGE_4KB;
	}
	while (user_heap_next > aligned) {
		user_heap_next -= PAGE_4KB;
		unmap_user_page(user_heap_next);
	}
	return user_heap_next;
}

/* Allocate a fresh per-process PDPT. PDPT[1..3] inherit the shared
 * kernel PDs; PDPT[0] stays zero so init_user_pgdir or copy_user_pgdir
 * can populate it. */
unsigned int *paging_create_user_pgdir(void)
{
	unsigned int phys = pmm_alloc();
	if (!phys)
		return 0;
	zero_frame(phys);
	pae_entry_t *pdpt = KVA(phys);
	wire_kernel_pdpt(pdpt);
	pdpt[0] = 0;
	return (unsigned int *)pdpt;
}

void paging_switch(unsigned int *pdpt_raw)
{
	if (!pdpt_raw)
		return;
	page_pdpt = (pae_entry_t *)pdpt_raw;
	__asm__ volatile ("mov %0, %%cr3" : :
	                  "r"(V2P((unsigned int)pdpt_raw)) : "memory");
}

void paging_destroy_user_pgdir(unsigned int *pdpt_raw)
{
	if (!pdpt_raw)
		return;
	pae_entry_t *pdpt = (pae_entry_t *)pdpt_raw;
	pae_entry_t pdpte = pdpt[0];
	if (pdpte & 1ULL) {
		unsigned int pd0_phys = (unsigned int)(pdpte & FRAME_MASK_4KB);
		pae_entry_t *pd0 = KVA(pd0_phys);
		unsigned int first = PD_IDX(USER_BASE);
		for (unsigned int p = 0; p < USER_PDES_2MB; p++) {
			pae_entry_t pde = pd0[first + p];
			if (!(pde & 1ULL))
				continue;
			if (pde & 0x80ULL)
				continue;                       /* huge -- skip (FB MMIO) */
			unsigned int pt_phys =
			    (unsigned int)(pde & FRAME_MASK_4KB);
			pae_entry_t *pt = KVA(pt_phys);
			for (unsigned int e = 0; e < PT_ENTRIES; e++) {
				pae_entry_t pte = pt[e];
				if (pte & 1ULL)
					pmm_free(
					    (unsigned int)(pte & FRAME_MASK_4KB));
			}
			pmm_free(pt_phys);
		}
		pmm_free(pd0_phys);
	}
	pmm_free(V2P((unsigned int)pdpt_raw));
}

int paging_user_range_ok(unsigned int addr, unsigned int len)
{
	if (len == 0)
		return 1;
	if (addr + len < addr)
		return 0;

	unsigned int end = addr + len;
	if (addr >= USER_CODE && end <= USER_STACK_TOP)
		return 1;
	if (addr >= USER_HEAP_LO && end <= user_heap_next)
		return 1;
	return 0;
}
