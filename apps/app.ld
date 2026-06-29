/* Link script for StinkOS userland apps. Produces a small ELF executable
 * whose loadable region starts at the user code address (0x400000).
 *
 * Three PT_LOAD program headers so the kernel ELF loader can stamp each
 * segment's user PTEs with the right W^X permissions (PF_X -> clear NX,
 * PF_W -> set RW):
 *   text   PF_R|PF_X     -- .text         (R-X)
 *   rodata PF_R          -- .rodata       (R-NX)
 *   data   PF_R|PF_W     -- .data + .bss  (RW-NX)
 *
 * Each PHDR is page-aligned so the loader can apply per-page perms
 * without spanning two segments. Refs: System V ABI Intel 386 supplement
 * (PHDR/section flags); Intel SDM Vol 3A §4.6 (PTE permissions). */
ENTRY(_start)

PHDRS
{
	text   PT_LOAD FLAGS(5);   /* R + X */
	rodata PT_LOAD FLAGS(4);   /* R only */
	data   PT_LOAD FLAGS(6);   /* R + W */
}

SECTIONS
{
	. = 0x400000;

	.text   : { *(.text*) }                 :text

	. = ALIGN(0x1000);
	.rodata : { *(.rodata*) }               :rodata

	. = ALIGN(0x1000);
	.data   : { *(.data*) }                 :data
	.bss    : { *(.bss*) *(COMMON) }        :data
}
