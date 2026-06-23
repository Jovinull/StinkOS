/* Link script for StinkOS userland apps. Produces a small ELF executable whose
 * single loadable region starts at the user code address (0x400000). The kernel
 * ELF loader reads the program headers and places the segment there. */
ENTRY(_start)

SECTIONS
{
	. = 0x400000;

	.text : { *(.text*) }
	.rodata : { *(.rodata*) }
	.data : { *(.data*) }
	.bss : { *(.bss*) *(COMMON) }
}
