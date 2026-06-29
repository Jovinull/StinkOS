/* Kernel linker script: kernel linked at virt 0x80100000 (higher-half),
 * loaded by bootmain at phys 0x100000 via AT() per section.
 *
 * bootmain reads e_entry from the ELF header and calls it as a function
 * pointer with paging OFF; kentry.s exports _kernel_phys_entry whose
 * VALUE is (virt addr of asm entry) - KERNBASE = the phys addr where
 * the section actually lives. The asm entry sets up a bootstrap pgdir
 * (identity-low + high-half mirror) and far-jumps to the higher-half
 * _kernel_start.
 *
 * The .multiboot section is forced FIRST so a Multiboot-1 loader (GRUB)
 * sees the magic header within the first 8 KiB of the file. Our own
 * bootmain ignores it (just an unused PT_LOAD payload).
 *
 * Refs: xv6-public/kernel.ld (canonical higher-half i386 script);
 * toaruos/kernel/arch/x86_64/link.ld (same AT()-per-section trick on
 * x86_64). */

ENTRY(_kernel_phys_entry)

PHDRS
{
	text PT_LOAD FLAGS(5);   /* R-X */
	data PT_LOAD FLAGS(6);   /* RW- */
}

SECTIONS
{
	. = 0x80100000;

	__kernel_text_start = .;
	.multiboot : AT(ADDR(.multiboot) - 0x80000000) {
		KEEP(*(.multiboot))
	} :text

	.text : AT(ADDR(.text) - 0x80000000) {
		*(.text)
		*(.text.*)
	} :text
	. = ALIGN(4096);
	__kernel_text_end = .;

	__kernel_rodata_start = .;
	.rodata : AT(ADDR(.rodata) - 0x80000000) {
		*(.rodata)
		*(.rodata.*)
	} :text
	. = ALIGN(4096);
	__kernel_rodata_end = .;

	__kernel_data_start = .;
	.data : AT(ADDR(.data) - 0x80000000) {
		*(.data)
		*(.data.*)
	} :data

	. = ALIGN(4);
	__bss_start = .;
	.bss : AT(ADDR(.bss) - 0x80000000) {
		*(.bss)
		*(.bss.*)
		*(COMMON)
	} :data
	. = ALIGN(4);
	__bss_end = .;
	__kernel_data_end = .;

	/DISCARD/ : {
		*(.comment)
		*(.note*)
		*(.eh_frame)
	}
}
