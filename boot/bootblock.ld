/* Bootblock linker script: tiny code that runs at 0x7C00, loads the
 * kernel ELF off disk via bootmain.c, and transfers control to it.
 *
 * The bootblock is a flat binary (no ELF wrapper). It must live entirely
 * below the kernel's link address (0x100000) and must not write into
 * 0x7C00 itself after bootmain copies the kernel there. STACK_TOP for
 * the bootblock is 0x90000 (well below 1 MiB, well above 0x7C00). */

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
