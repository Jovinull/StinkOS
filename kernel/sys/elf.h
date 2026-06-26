/* ELF32 streaming loader: reads an i386 executable from a raw disk slot, copies
 * each loadable segment into the already-mapped user address space, and returns
 * the entry-point virtual address. No kernel staging buffer -- the app's size
 * is bounded only by the user code window and the slot's sector count. */
#ifndef ELF_H
#define ELF_H

/* Loads the ELF image stored at LBA 'lba' (occupying 'sectors' contiguous
 * 512-byte sectors). On success returns 0 and writes the entry virtual address
 * to *entry. Returns non-zero if the image is malformed, a segment falls
 * outside the user code window, or a segment runs past the slot. */
int elf_load(unsigned int lba, unsigned int sectors, unsigned int *entry);

#endif
