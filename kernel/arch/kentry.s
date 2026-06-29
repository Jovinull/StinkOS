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

	# CR4.PSE so the bootstrap pgdir's 4 MiB PDEs are honored.
	mov %cr4, %eax
	or  $0x10, %eax
	mov %eax, %cr4

	# CR3 = phys of bootstrap pgdir.
	mov $(bootstrap_pgdir - KERNBASE), %eax
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

# Bootstrap page directory. 1024 PDEs of 4 MiB PSE entries.
# Layout chosen so the CPU can keep fetching from both LOW phys (during
# the moment between CR0.PG = 1 and the indirect jump to high virt) AND
# from the HIGH-half virt after the jump:
#   PDE[0..511]      identity-map [0, 2 GiB)        (PG | RW | PS)
#   PDE[512..575]    high-half mirror of phys [0, 256 MB) -> virt [KERNBASE, +256MB)
#   PDE[576..1023]   identity-map [2.25 GiB, 4 GiB) (so MMIO above
#                    DEVSPACE keeps virt = phys)
.section .data
.align 4096
.global bootstrap_pgdir
bootstrap_pgdir:
	# PDE[0..511] identity [0, 2 GiB)
	.set i, 0
	.rept 512
		.long (i * 0x400000) | 0x83
		.set i, i + 1
	.endr
	# PDE[512..575] high-half mirror [0, 256 MB) at virt KERNBASE
	.set i, 0
	.rept 64
		.long (i * 0x400000) | 0x83
		.set i, i + 1
	.endr
	# PDE[576..1023] identity continues at [576*4MB, 4GB)
	.set i, 576
	.rept 1024 - 576
		.long (i * 0x400000) | 0x83
		.set i, i + 1
	.endr
