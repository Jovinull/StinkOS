/* Minimal ELF32 definitions the boot stage 2 (bootmain.c) needs.
 *
 * NOT YET WIRED INTO THE BUILD. This header is part of the ELF-aware
 * bootloader groundwork tracked in TODO §13. Until the bootmain code
 * is plumbed through Makefile + boot.s, the production kernel image
 * is still loaded as a flat binary by boot.s itself.
 *
 * Field layout follows the ELF v1 spec (System V ABI, Intel 386
 * supplement); we only declare the subset bootmain.c actually reads. */

#ifndef BOOT_ELF32_H
#define BOOT_ELF32_H

/* p_type values we care about. */
#define PT_LOAD 1

struct elf32_ehdr {
	unsigned char  e_ident[16];
	unsigned short e_type;
	unsigned short e_machine;
	unsigned int   e_version;
	unsigned int   e_entry;
	unsigned int   e_phoff;
	unsigned int   e_shoff;
	unsigned int   e_flags;
	unsigned short e_ehsize;
	unsigned short e_phentsize;
	unsigned short e_phnum;
	unsigned short e_shentsize;
	unsigned short e_shnum;
	unsigned short e_shstrndx;
} __attribute__((packed));

struct elf32_phdr {
	unsigned int p_type;
	unsigned int p_offset;
	unsigned int p_vaddr;
	unsigned int p_paddr;
	unsigned int p_filesz;
	unsigned int p_memsz;
	unsigned int p_flags;
	unsigned int p_align;
} __attribute__((packed));

#endif
