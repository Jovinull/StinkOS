/* Flat binary layout for StinkOS.
 * boot.o is listed first on the linker command line, so its .text (with _start
 * at 0x7C00 and pm_entry at 0x7E00) lands at the base. .bss is not stored in the
 * image; pm_entry zeroes it at runtime using the symbols exported here. */

ENTRY(_start)

SECTIONS
{
	. = 0x7C00;

	.text : {
		*(.text)
		*(.text.*)
	}

	.rodata : {
		*(.rodata)
		*(.rodata.*)
	}

	.data : {
		*(.data)
		*(.data.*)
	}

	. = ALIGN(4);
	__bss_start = .;
	.bss : {
		*(.bss)
		*(.bss.*)
		*(COMMON)
	}
	. = ALIGN(4);
	__bss_end = .;

	/DISCARD/ : {
		*(.comment)
		*(.note*)
		*(.eh_frame)
	}
}
