/* Flat binary layout for StinkOS.
 * boot.o is listed first on the linker command line, so its .text (with _start
 * at 0x7C00 and pm_entry at 0x7E00) lands at the base. .bss is not stored in the
 * image; pm_entry zeroes it at runtime using the symbols exported here.
 *
 * The PHDRS declaration below is harmless for the current `--oformat binary`
 * output (binary mode dumps raw section bytes, ignoring program headers) but
 * makes the parallel kernel.elf artifact carry well-formed PT_LOAD entries
 * with correct vaddr/paddr/filesz/memsz so a future ELF-aware bootloader (see
 * TODO §13) can walk them. Until §13 lands, kernel.elf is diagnostic only --
 * the production boot path still uses the flat os.bin. */

ENTRY(_start)

PHDRS
{
	text PT_LOAD FLAGS(5);   /* R-X */
	data PT_LOAD FLAGS(6);   /* RW- */
}

SECTIONS
{
	. = 0x7C00;

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
