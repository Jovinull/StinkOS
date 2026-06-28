# Kernel entry. bootmain (in boot/bootmain.c) jumps here after copying
# every PT_LOAD segment to its linked address; the ELF header's
# e_entry is set by boot/kernel.ld via ENTRY(_kernel_start).
#
# At entry: protected mode is on, GDT is the flat one set up in
# boot/boot.s (CS=0x08, DS=0x10), ESP points somewhere inside the
# bootblock's stack. We are about to throw the bootblock memory away
# so we cannot trust that stack -- reset ESP to our own STACK_TOP
# (defined in kernel/arch/memlayout.h, shared with the bootblock).
#
# Then we zero the kernel's .bss range (symbols come from kernel.ld)
# and call kmain. kmain must not return; if it does, halt.

# Must match STACK_TOP in kernel/arch/memlayout.h and boot/boot.s.
.equ STACK_TOP, 0x90000

.code32
.global _kernel_start
_kernel_start:
	mov $0x10, %ax
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs
	mov %ax, %ss
	mov $STACK_TOP, %esp

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
