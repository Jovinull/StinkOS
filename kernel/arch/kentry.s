# Kernel entry. bootmain (in boot/bootmain.c) jumps to the address stored
# in the ELF's e_entry. With higher-half link, the kernel image lives at
# virt 0x80100000+ (kernel.ld), but bootmain runs with paging OFF; we
# therefore set the ELF entry to the PHYSICAL address of _phys_entry --
# evaluated as `(_phys_entry's virt) - KERNBASE` -- so the indirect call
# from bootmain lands at the right physical bytes.
#
# Flow:
#   1. bootmain copies every PT_LOAD to its p_paddr (= VMA - KERNBASE)
#      and jumps to e_entry (= phys address of _phys_entry).
#   2. _phys_entry runs at LOW phys, paging OFF. Enables CR4.PSE, loads
#      CR3 with the bootstrap pgdir, turns on CR0.PG. Now paging on; CPU
#      is still fetching from low phys (identity mapped in bootstrap pgdir).
#   3. Indirect jump to _kernel_start. Linker resolved that to virt
#      0x80100000+, so the jump transfers control to high-half virt.
#   4. _kernel_start zeros BSS, sets ESP to STACK_TOP_VIRT, calls kmain.
#      kmain stays in higher-half forever; paging.c later drops the
#      bootstrap low-identity entries.
#
# Refs:
#   PRIMARY  xv6-public/entry.S: same `entry = V2P_WO(entry)` indirection,
#       same single-PSE bootstrap pgdir, same indirect call to main()
#       to force the assembler to emit an absolute address instead of
#       PC-relative (which would stay in low phys).
#   CONTRAST toaruos/kernel/arch/x86_64/boot.S: 64-bit higher-half with
#       4-level paging built statically. Same concept, different tables.

.equ KERNBASE,        0x80000000
.equ STACK_TOP_PHYS,  0x00090000
.equ STACK_TOP_VIRT,  STACK_TOP_PHYS + KERNBASE
.equ MSR_EFER,        0xC0000080
.equ EFER_NXE,        0x800

.code32

# Externally-visible entry symbol -- evaluated to PHYS so bootmain's
# `entry()` (an indirect call via ELF e_entry) lands at the right
# bytes while paging is still off.
.global _kernel_phys_entry
.set    _kernel_phys_entry, _phys_entry - KERNBASE

.section .text

_phys_entry:
	# Enable IA32_EFER.NXE so the PAE walker honours PTE bit 63 (NX)
	# once the v0.5 switch lands. Guarded by CPUID extended-leaf NX
	# bit (EAX=0x80000001 EDX bit 20): on a CPU that doesn't advertise
	# NX, writing EFER.NXE would either #GP (MSR reserved) or be silently
	# ignored. Skipping the WRMSR keeps boot working on legacy CPUs +
	# QEMU TCG defaults without `-cpu` flag; with NX, the bit goes live
	# immediately so paging.c's PAE rewrite can stamp PTE bit 63 from
	# its very first PTE.
	#
	# Refs:
	#   serenity/Kernel/Arch/x86_64/Boot/ap_setup.S:62-74 -- same
	#       MSR/bit pattern in asm, copy-paste semantics across
	#       PAE i386 and long mode (their CPUID guard lives in C
	#       at Processor.cpp:537-541; we keep it asm-side for boot).
	#   Intel SDM Vol 2A "CPUID" (extended leaf 0x80000001 EDX bit 20)
	#       + Vol 3A §4.6.2 (IA32_EFER.NXE) + Vol 4 §2.2.1
	#       (MSR_IA32_EFER = 0xC0000080).
	mov $0x80000000, %eax
	cpuid
	cmp $0x80000001, %eax
	jb  .Lnxe_skip
	mov $0x80000001, %eax
	cpuid
	test $(1 << 20), %edx
	jz  .Lnxe_skip
	mov $MSR_EFER, %ecx
	rdmsr
	or  $EFER_NXE, %eax
	wrmsr
.Lnxe_skip:

	# CR4 = PSE (bit 4) | PAE (bit 5). PAE switches the page-walker to
	# 3-level (PDPT -> PD -> PT) with 8-byte entries; PSE under PAE
	# means PD entries with PS=1 map 2 MiB instead of 4 MiB. Both bits
	# must be set BEFORE CR0.PG goes on so the CPU reads CR3 as a
	# PDPT pointer when paging flips live.
	mov %cr4, %eax
	or  $0x30, %eax
	mov %eax, %cr4

	# CR3 = phys of bootstrap PDPT (32-byte structure, 4 KiB-aligned
	# here for convenience even though PAE only requires 32-byte
	# alignment).
	mov $(bootstrap_pdpt - KERNBASE), %eax
	mov %eax, %cr3

	# CR0.PG -- paging on.
	mov %cr0, %eax
	or  $0x80000000, %eax
	mov %eax, %cr0

	# Indirect absolute jump into the higher half. A direct `jmp
	# _kernel_start` would assemble to a PC-relative offset that
	# stays in low phys; loading the address into a register first
	# forces an absolute transfer.
	mov $_kernel_start, %eax
	jmp *%eax

.global _kernel_start
_kernel_start:
	mov $0x10, %ax
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs
	mov %ax, %ss
	mov $STACK_TOP_VIRT, %esp

	cld
	mov $__bss_start, %edi
	mov $__bss_end, %ecx
	sub %edi, %ecx
	xor %eax, %eax
	rep stosb

	call kmain
hang:
	cli
	hlt
	jmp hang

# Bootstrap PAE paging structures.
#
# Layout:
#   CR3 -> bootstrap_pdpt (4 entries x 8 bytes)
#     PDPT[0] -> bootstrap_pd0 -> identity [0, 1 GiB)        (PSE 2 MiB)
#     PDPT[1] -> bootstrap_pd1 -> identity [1, 2 GiB)        (PSE 2 MiB)
#     PDPT[2] -> bootstrap_pd2 -> high-half mirror [KERNBASE,
#                                  KERNBASE+1 GiB) -> phys [0, 1 GiB)
#     PDPT[3] -> bootstrap_pd3 -> identity [3, 4 GiB)        (PSE 2 MiB)
#
# Identity-low (PD0) keeps EIP valid between CR0.PG=1 and the indirect
# jump to high virt. High-half mirror (PD2) is what the C kernel lives
# in. DEVSPACE identity (PD3) covers MMIO above 0xC0000000 (LFB at
# 0xFD000000, e1000 BAR ~0xFEB80000). paging.c rebuilds a runtime
# pgdir in paging_init and drops PD0 entirely once paging is live.
#
# PDPT entries (Intel SDM Vol 3A Table 4-8) have NO RW/US bits -- the
# protection lives in PD/PT entries; PDPT carries only P (bit 0).
#
# PSE PAE PDE format (Intel SDM Vol 3A Table 4-9): bits 0-2 P|RW|US,
# bit 7 PS=1 to mark the 2 MiB huge mapping, bits 21+ are the 2 MiB-
# aligned phys frame, bit 63 is NX.
.section .data
.balign 4096
.global bootstrap_pdpt
bootstrap_pdpt:
	# `+ 1` instead of `| 1` because GAS rejects bitwise ops on
	# relocatable symbol differences. The PD frames are 4 KiB-aligned
	# so the low 12 bits are zero -- `+` and `|` produce identical
	# bytes for the present-bit set.
	.long (bootstrap_pd0 - KERNBASE) + 1
	.long 0
	.long (bootstrap_pd1 - KERNBASE) + 1
	.long 0
	.long (bootstrap_pd2 - KERNBASE) + 1
	.long 0
	.long (bootstrap_pd3 - KERNBASE) + 1
	.long 0

.balign 4096
bootstrap_pd0:
	# 512 x 2 MiB PSE entries identity-mapping phys [0, 1 GiB).
	.set i, 0
	.rept 512
		.long (i * 0x200000) | 0x83
		.long 0
		.set i, i + 1
	.endr

.balign 4096
bootstrap_pd1:
	# identity [1 GiB, 2 GiB)
	.set i, 0
	.rept 512
		.long ((i + 512) * 0x200000) | 0x83
		.long 0
		.set i, i + 1
	.endr

.balign 4096
bootstrap_pd2:
	# high-half mirror: virt [KERNBASE, KERNBASE+1 GiB) -> phys [0, 1 GiB)
	.set i, 0
	.rept 512
		.long (i * 0x200000) | 0x83
		.long 0
		.set i, i + 1
	.endr

.balign 4096
bootstrap_pd3:
	# identity [3 GiB, 4 GiB) -- covers DEVSPACE MMIO at 0xFD000000+
	# (LFB, e1000 BAR, etc).
	.set i, 0
	.rept 512
		.long ((i + 1536) * 0x200000) | 0x83
		.long 0
		.set i, i + 1
	.endr
