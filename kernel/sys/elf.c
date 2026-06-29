/* ELF32 streaming loader. Reads the ELF header and program headers from disk,
 * then pulls each PT_LOAD segment's file content directly into the already-
 * mapped user address space -- no kernel staging buffer, no hard cap on app
 * size beyond the user code window itself. The .bss tail of each segment is
 * zero-filled, and every segment is bounds-checked against both the file
 * length (slot size on disk) and the user code virtual range, so a malformed
 * ELF cannot drive the kernel into reading past the slot or writing outside
 * the user region. */
#include "elf.h"
#include "ata.h"
#include "paging.h"

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

/* Same byte layout as boot/elf32.h. The two copies stay in sync by hand:
 * the bootloader can't pull in kernel typedefs (it runs before paging /
 * before any kernel code), and the kernel can't pull in boot/ headers
 * (different include strategy). Both follow the System V ABI Intel 386
 * supplement -- if you change one, change the other. */
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

/* Layout drift safety net (matches boot/elf32.h). */
_Static_assert(sizeof(struct elf32_ehdr) == 52, "elf32_ehdr must be 52 bytes");
_Static_assert(sizeof(struct elf32_phdr) == 32, "elf32_phdr must be 32 bytes");

#define ET_EXEC      2
#define EM_386       3
#define PT_LOAD      1
#define ELFCLASS32   1
#define ELFDATA2LSB  1
#define PF_X         1
#define PF_W         2
#define PF_R         4

#define SECTOR_SIZE  512u
#define MAX_PHDRS_BYTES 4096u            /* generous: a typical app has 2-3 phdrs */
#define READ_CHUNK_SECTORS 64u           /* per-call cap for aligned bulk reads */

/* Bounce buffer for unaligned head/tail reads (file offsets that don't sit on
 * a sector boundary). Also reused to slurp the ELF header sector. */
static u8 sector_buf[SECTOR_SIZE];

/* Holds the program-header table after read_at pulls it in. Sized to fit any
 * realistic ELF (1000+ tiny phdrs); a malformed ELF asking for more is
 * rejected before any disk read. */
static u8 phdr_buf[MAX_PHDRS_BYTES];

/* Read 'n' bytes starting at byte offset 'file_off' from the ELF image stored
 * at LBA 'image_lba' into 'dst'. The aligned middle of the request is pulled
 * straight into 'dst' in multi-sector chunks; the head and tail (when not
 * sector-aligned) bounce through sector_buf. Returns 0 on success. */
static int read_at(u32 image_lba, u32 file_off, void *dst, u32 n)
{
	u8 *out = (u8 *)dst;
	while (n > 0) {
		u32 sec        = image_lba + file_off / SECTOR_SIZE;
		u32 off_in_sec = file_off % SECTOR_SIZE;

		if (off_in_sec == 0 && n >= SECTOR_SIZE) {
			u32 sectors = n / SECTOR_SIZE;
			if (sectors > READ_CHUNK_SECTORS)
				sectors = READ_CHUNK_SECTORS;
			if (ata_read(sec, sectors, out) != 0)
				return -1;
			u32 bytes = sectors * SECTOR_SIZE;
			out      += bytes;
			file_off += bytes;
			n        -= bytes;
		} else {
			if (ata_read(sec, 1, sector_buf) != 0)
				return -1;
			u32 take = SECTOR_SIZE - off_in_sec;
			if (take > n)
				take = n;
			for (u32 k = 0; k < take; k++)
				out[k] = sector_buf[off_in_sec + k];
			out      += take;
			file_off += take;
			n        -= take;
		}
	}
	return 0;
}

int elf_load(unsigned int lba, unsigned int sectors, unsigned int *entry)
{
	u32 image_bytes = sectors * SECTOR_SIZE;

	/* ---- 1. ELF header (first sector) ---- */
	if (ata_read(lba, 1, sector_buf) != 0)
		return 1;
	struct elf32_ehdr *eh = (struct elf32_ehdr *)sector_buf;

	if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
	    eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F')
		return 1;
	if (eh->e_ident[4] != ELFCLASS32 || eh->e_ident[5] != ELFDATA2LSB)
		return 1;
	if (eh->e_type != ET_EXEC || eh->e_machine != EM_386)
		return 1;
	if (eh->e_phoff == 0 || eh->e_phentsize < sizeof(struct elf32_phdr))
		return 1;

	u32 code_lo = paging_user_code();
	u32 code_hi = paging_user_code_end();
	if (eh->e_entry < code_lo || eh->e_entry >= code_hi)
		return 1;

	/* Save the fields we still need after sector_buf is clobbered. */
	u16 e_phnum     = eh->e_phnum;
	u16 e_phentsize = eh->e_phentsize;
	u32 e_phoff     = eh->e_phoff;
	u32 e_entry     = eh->e_entry;

	/* ---- 2. Program-header table ---- */
	u32 ph_total = (u32)e_phnum * e_phentsize;
	if (ph_total > MAX_PHDRS_BYTES)
		return 1;
	if (e_phoff >= image_bytes || ph_total > image_bytes - e_phoff)
		return 1;
	if (read_at(lba, e_phoff, phdr_buf, ph_total) != 0)
		return 1;

	/* ---- 3. Stream each PT_LOAD into user memory ---- */
	int entry_loaded = 0;
	for (u16 i = 0; i < e_phnum; i++) {
		struct elf32_phdr *ph =
			(struct elf32_phdr *)(phdr_buf + (u32)i * e_phentsize);

		if (ph->p_type != PT_LOAD)
			continue;
		if (ph->p_memsz == 0)
			continue;
		if (ph->p_filesz > ph->p_memsz)
			return 1;

		/* File bounds: don't read past the slot's last sector. */
		if (ph->p_filesz > 0) {
			if (ph->p_offset >= image_bytes)
				return 1;
			if (ph->p_filesz > image_bytes - ph->p_offset)
				return 1;
		}

		/* Virtual bounds: must land inside the user code window. */
		if (ph->p_vaddr < code_lo || ph->p_vaddr >= code_hi)
			return 1;
		if (ph->p_memsz > code_hi - ph->p_vaddr)
			return 1;

		if (e_entry >= ph->p_vaddr && e_entry < ph->p_vaddr + ph->p_memsz)
			entry_loaded = 1;

		u8 *dst = (u8 *)ph->p_vaddr;
		if (ph->p_filesz > 0)
			if (read_at(lba, ph->p_offset, dst, ph->p_filesz) != 0)
				return 1;
		for (u32 k = ph->p_filesz; k < ph->p_memsz; k++)
			dst[k] = 0;

		/* Now that the segment bytes are in place, downgrade its PTEs
		 * to the W^X permissions declared by p_flags. Done last so the
		 * loader's own writes above always succeed against the default
		 * RW mapping established by paging_init_user_pgdir. */
		paging_user_set_segment_perms(ph->p_vaddr, ph->p_memsz,
		                              (ph->p_flags & PF_X) != 0,
		                              (ph->p_flags & PF_W) != 0);
	}

	if (!entry_loaded)
		return 1;
	*entry = e_entry;
	return 0;
}
