/* Minimal ELF32 program loader: validates an i386 executable image and copies
 * its loadable segments into memory, returning the entry point. */
#ifndef ELF_H
#define ELF_H

/* Loads the ELF image in 'image' (of 'size' bytes). On success returns 0 and
 * writes the entry virtual address to *entry. Returns non-zero if the image is
 * malformed or a segment falls outside the allowed user code region. */
int elf_load(const void *image, unsigned int size, unsigned int *entry);

#endif
