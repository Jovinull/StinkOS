/* ELF32 loader. Parses the standard executable header and program headers and
 * copies each PT_LOAD segment to its virtual address, zero-filling the part that
 * is not present in the file (.bss). Every segment is bounds-checked against the
 * mapped user code region so a malformed app cannot scribble over the kernel. */
#include "elf.h"
#include "paging.h"

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

struct elf32_ehdr {
	u8  e_ident[16];
	u16 e_type;
	u16 e_machine;
	u32 e_version;
	u32 e_entry;
	u32 e_phoff;
	u32 e_shoff;
	u32 e_flags;
	u16 e_ehsize;
	u16 e_phentsize;
	u16 e_phnum;
	u16 e_shentsize;
	u16 e_shnum;
	u16 e_shstrndx;
} __attribute__((packed));

struct elf32_phdr {
	u32 p_type;
	u32 p_offset;
	u32 p_vaddr;
	u32 p_paddr;
	u32 p_filesz;
	u32 p_memsz;
	u32 p_flags;
	u32 p_align;
} __attribute__((packed));

#define ET_EXEC   2
#define EM_386    3
#define PT_LOAD   1
#define ELFCLASS32 1
#define ELFDATA2LSB 1

static void copy(u8 *dst, const u8 *src, u32 n)
{
	for (u32 i = 0; i < n; i++)
		dst[i] = src[i];
}

static void zero(u8 *dst, u32 n)
{
	for (u32 i = 0; i < n; i++)
		dst[i] = 0;
}

int elf_load(const void *image, unsigned int size, unsigned int *entry)
{
	const u8 *base = (const u8 *)image;

	if (size < sizeof(struct elf32_ehdr))
		return 1;

	const struct elf32_ehdr *eh = (const struct elf32_ehdr *)base;

	if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
	    eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F')
		return 1;
	if (eh->e_ident[4] != ELFCLASS32 || eh->e_ident[5] != ELFDATA2LSB)
		return 1;
	if (eh->e_type != ET_EXEC || eh->e_machine != EM_386)
		return 1;
	if (eh->e_phoff == 0 || eh->e_phentsize < sizeof(struct elf32_phdr))
		return 1;

	const u32 code_lo = paging_user_code();
	const u32 code_hi = paging_user_code_end();

	if (eh->e_entry < code_lo || eh->e_entry >= code_hi)
		return 1;

	for (u16 i = 0; i < eh->e_phnum; i++) {
		u32 off = eh->e_phoff + (u32)i * eh->e_phentsize;
		/* Subtraction form throughout: never add two u32 that could wrap. */
		if (off > size || sizeof(struct elf32_phdr) > size - off)
			return 1;

		const struct elf32_phdr *ph =
			(const struct elf32_phdr *)(base + off);

		if (ph->p_type != PT_LOAD)
			continue;
		if (ph->p_memsz == 0)
			continue;
		if (ph->p_filesz > ph->p_memsz)
			return 1;
		if (ph->p_offset > size || ph->p_filesz > size - ph->p_offset)
			return 1;

		/* Segment must land entirely inside the mapped user code window. */
		if (ph->p_vaddr < code_lo || ph->p_vaddr >= code_hi)
			return 1;
		if (ph->p_memsz > code_hi - ph->p_vaddr)
			return 1;

		u8 *dst = (u8 *)ph->p_vaddr;
		copy(dst, base + ph->p_offset, ph->p_filesz);
		zero(dst + ph->p_filesz, ph->p_memsz - ph->p_filesz);
	}

	*entry = eh->e_entry;
	return 0;
}
