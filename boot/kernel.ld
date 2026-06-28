/* Kernel linker script: kernel linked at 0x100000 (1 MiB), ELF output
 * with proper PT_LOAD program headers. bootmain.c walks those phdrs
 * and copies each PT_LOAD segment to its p_paddr, then jumps to
 * the ELF entry point (which we set to _kernel_start in kentry.s).
 *
 * The .multiboot section is forced FIRST in the file/RAM image so
 * a Multiboot-1 loader (e.g. GRUB) sees the magic header within the
 * first 8 KiB and treats us as a bootable kernel. Our own bootmain
 * ignores it (just an unused PT_LOAD payload). */

ENTRY(_kernel_start)

PHDRS
{
	text PT_LOAD FLAGS(5);   /* R-X */
	data PT_LOAD FLAGS(6);   /* RW- */
}

SECTIONS
{
	. = 0x100000;

	.multiboot : {
		KEEP(*(.multiboot))
	} :text

	.text : {
		*(.text)
		*(.text.*)
	} :text

	.rodata : {
		*(.rodata)
		*(.rodata.*)
	} :text

	.data : {
		*(.data)
		*(.data.*)
	} :data

	. = ALIGN(4);
	__bss_start = .;
	.bss : {
		*(.bss)
		*(.bss.*)
		*(COMMON)
	} :data
	. = ALIGN(4);
	__bss_end = .;

	/DISCARD/ : {
		*(.comment)
		*(.note*)
		*(.eh_frame)
	}
}
