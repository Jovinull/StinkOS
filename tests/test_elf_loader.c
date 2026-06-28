/* Host-side test for the ELF32 validation gates in
 * kernel/sys/elf.c. The loader does ~12 separate checks before it
 * trusts any byte from the image; a regression in any one of them
 * either makes a malformed ELF crash the kernel (boot fail) or, in
 * the worst case, lets a user-controlled phdr write outside the
 * user code window (sandbox escape).
 *
 * We mirror the validation pipeline against an in-memory image
 * (header + phdr table + segments) and assert which gate fires for
 * which malformed input. Disk I/O is bypassed since the data is
 * already in memory.
 */
#include <stdio.h>
#include <string.h>

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

#define ET_EXEC      2
#define EM_386       3
#define PT_LOAD      1
#define ELFCLASS32   1
#define ELFDATA2LSB  1

#define SECTOR_SIZE       512u
#define MAX_PHDRS_BYTES   4096u
#define CODE_LO           0x00800000u
#define CODE_HI           0x01000000u

/* Validation result codes. >0 = which gate fired; 0 = success. */
enum {
	OK              = 0,
	FAIL_MAGIC      = 1,
	FAIL_CLASS      = 2,
	FAIL_TYPE       = 3,
	FAIL_PHOFF      = 4,
	FAIL_ENTRY_RANGE= 5,
	FAIL_PHTOTAL_CAP= 6,
	FAIL_PHTABLE_OOB= 7,
	FAIL_FILE_GT_MEM= 8,
	FAIL_SEG_OFFSET = 9,
	FAIL_SEG_VADDR  = 10,
	FAIL_SEG_MEMSZ  = 11,
	FAIL_ENTRY_ORPH = 12
};

/* Mirror of elf_load validation chain. `image` is the whole on-disk
 * payload in memory; `image_bytes` is its size. Returns OK or the
 * gate that rejected. */
static int validate(const u8 *image, u32 image_bytes)
{
	if (image_bytes < sizeof(struct elf32_ehdr)) return FAIL_MAGIC;
	const struct elf32_ehdr *eh = (const struct elf32_ehdr *)image;

	if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
	    eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F')
		return FAIL_MAGIC;
	if (eh->e_ident[4] != ELFCLASS32 || eh->e_ident[5] != ELFDATA2LSB)
		return FAIL_CLASS;
	if (eh->e_type != ET_EXEC || eh->e_machine != EM_386)
		return FAIL_TYPE;
	if (eh->e_phoff == 0 || eh->e_phentsize < sizeof(struct elf32_phdr))
		return FAIL_PHOFF;

	if (eh->e_entry < CODE_LO || eh->e_entry >= CODE_HI)
		return FAIL_ENTRY_RANGE;

	u32 ph_total = (u32)eh->e_phnum * eh->e_phentsize;
	if (ph_total > MAX_PHDRS_BYTES) return FAIL_PHTOTAL_CAP;
	if (eh->e_phoff >= image_bytes ||
	    ph_total > image_bytes - eh->e_phoff)
		return FAIL_PHTABLE_OOB;

	int entry_loaded = 0;
	for (u16 i = 0; i < eh->e_phnum; i++) {
		const struct elf32_phdr *ph =
		    (const struct elf32_phdr *)(image + eh->e_phoff +
		                                (u32)i * eh->e_phentsize);
		if (ph->p_type != PT_LOAD) continue;
		if (ph->p_memsz == 0)      continue;

		if (ph->p_filesz > ph->p_memsz) return FAIL_FILE_GT_MEM;

		if (ph->p_filesz > 0) {
			if (ph->p_offset >= image_bytes)            return FAIL_SEG_OFFSET;
			if (ph->p_filesz > image_bytes - ph->p_offset) return FAIL_SEG_OFFSET;
		}
		if (ph->p_vaddr < CODE_LO || ph->p_vaddr >= CODE_HI)
			return FAIL_SEG_VADDR;
		if (ph->p_memsz > CODE_HI - ph->p_vaddr)
			return FAIL_SEG_MEMSZ;

		if (eh->e_entry >= ph->p_vaddr &&
		    eh->e_entry <  ph->p_vaddr + ph->p_memsz)
			entry_loaded = 1;
	}

	if (!entry_loaded) return FAIL_ENTRY_ORPH;
	return OK;
}

/* Build a minimal valid ELF in `buf`. Returns image_bytes. The
 * caller may then sabotage one field at a time and re-validate. */
static u32 build_good_elf(u8 *buf, u32 entry_va)
{
	memset(buf, 0, 2048);
	struct elf32_ehdr *eh = (struct elf32_ehdr *)buf;
	eh->e_ident[0] = 0x7F;
	eh->e_ident[1] = 'E';
	eh->e_ident[2] = 'L';
	eh->e_ident[3] = 'F';
	eh->e_ident[4] = ELFCLASS32;
	eh->e_ident[5] = ELFDATA2LSB;
	eh->e_type      = ET_EXEC;
	eh->e_machine   = EM_386;
	eh->e_entry     = entry_va;
	eh->e_phoff     = 64;
	eh->e_phentsize = sizeof(struct elf32_phdr);
	eh->e_phnum     = 1;

	struct elf32_phdr *ph = (struct elf32_phdr *)(buf + 64);
	ph->p_type   = PT_LOAD;
	ph->p_offset = 256;
	ph->p_vaddr  = CODE_LO;
	ph->p_filesz = 512;
	ph->p_memsz  = 1024;        /* tail bss */
	return 2048;
}

static int expect_int(const char *label, int got, int want)
{
	if (got == want) { printf("ok   %-55s = %d\n", label, got); return 0; }
	printf("FAIL %s: got %d, want %d\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;
	u8 buf[4096];
	u32 n;

	/* --- baseline: a well-formed ELF passes every gate. -------- */
	n = build_good_elf(buf, CODE_LO + 0);
	failures += expect_int("well-formed ELF -> OK", validate(buf, n), OK);

	/* --- magic ------------------------------------------------- */
	n = build_good_elf(buf, CODE_LO);
	buf[1] = 'X';
	failures += expect_int("bad magic byte -> FAIL_MAGIC",
	                       validate(buf, n), FAIL_MAGIC);

	/* --- short image: header doesn't even fit ---------------- */
	failures += expect_int("image_bytes=8 -> FAIL_MAGIC",
	                       validate(buf, 8), FAIL_MAGIC);

	/* --- class: 64-bit binary rejected ---------------------- */
	n = build_good_elf(buf, CODE_LO);
	((struct elf32_ehdr *)buf)->e_ident[4] = 2;     /* ELFCLASS64 */
	failures += expect_int("64-bit class -> FAIL_CLASS",
	                       validate(buf, n), FAIL_CLASS);

	/* --- type: ET_DYN (shared object) rejected ------------- */
	n = build_good_elf(buf, CODE_LO);
	((struct elf32_ehdr *)buf)->e_type = 3;          /* ET_DYN */
	failures += expect_int("ET_DYN -> FAIL_TYPE",
	                       validate(buf, n), FAIL_TYPE);

	/* --- phoff zero -------------------------------------- */
	n = build_good_elf(buf, CODE_LO);
	((struct elf32_ehdr *)buf)->e_phoff = 0;
	failures += expect_int("phoff=0 -> FAIL_PHOFF",
	                       validate(buf, n), FAIL_PHOFF);

	/* --- phentsize smaller than struct ---------------- */
	n = build_good_elf(buf, CODE_LO);
	((struct elf32_ehdr *)buf)->e_phentsize = 16;    /* < 32 */
	failures += expect_int("tiny phentsize -> FAIL_PHOFF",
	                       validate(buf, n), FAIL_PHOFF);

	/* --- entry outside code window ----------------- */
	n = build_good_elf(buf, 0x42);                   /* low canonical addr */
	failures += expect_int("entry below CODE_LO -> FAIL_ENTRY_RANGE",
	                       validate(buf, n), FAIL_ENTRY_RANGE);
	n = build_good_elf(buf, CODE_HI);                /* exactly at hi */
	failures += expect_int("entry == CODE_HI -> FAIL_ENTRY_RANGE",
	                       validate(buf, n), FAIL_ENTRY_RANGE);

	/* --- absurd e_phnum overflows MAX_PHDRS_BYTES -------- */
	n = build_good_elf(buf, CODE_LO);
	((struct elf32_ehdr *)buf)->e_phnum = 200;       /* 200 * 32 = 6400 > 4096 */
	failures += expect_int("phnum=200 -> FAIL_PHTOTAL_CAP",
	                       validate(buf, n), FAIL_PHTOTAL_CAP);

	/* --- phoff way past EOF ------------------------- */
	n = build_good_elf(buf, CODE_LO);
	((struct elf32_ehdr *)buf)->e_phoff = 99999;
	failures += expect_int("phoff past image -> FAIL_PHTABLE_OOB",
	                       validate(buf, n), FAIL_PHTABLE_OOB);

	/* --- ph table tail past EOF ------------------- */
	n = build_good_elf(buf, CODE_LO);
	((struct elf32_ehdr *)buf)->e_phoff = n - 16;    /* ph_total=32, runs off end */
	failures += expect_int("phoff+ph_total > EOF -> FAIL_PHTABLE_OOB",
	                       validate(buf, n), FAIL_PHTABLE_OOB);

	/* --- filesz > memsz (would zero-fill negative) ---- */
	n = build_good_elf(buf, CODE_LO);
	{
		struct elf32_phdr *ph = (struct elf32_phdr *)(buf + 64);
		ph->p_filesz = ph->p_memsz + 1;
	}
	failures += expect_int("filesz>memsz -> FAIL_FILE_GT_MEM",
	                       validate(buf, n), FAIL_FILE_GT_MEM);

	/* --- segment offset reads past EOF -------------- */
	n = build_good_elf(buf, CODE_LO);
	{
		struct elf32_phdr *ph = (struct elf32_phdr *)(buf + 64);
		ph->p_offset = 9999;
	}
	failures += expect_int("seg p_offset > EOF -> FAIL_SEG_OFFSET",
	                       validate(buf, n), FAIL_SEG_OFFSET);

	/* --- segment vaddr below user window ------------ */
	n = build_good_elf(buf, CODE_LO);
	{
		struct elf32_phdr *ph = (struct elf32_phdr *)(buf + 64);
		ph->p_vaddr = 0x100;
	}
	failures += expect_int("seg vaddr below CODE_LO -> FAIL_SEG_VADDR",
	                       validate(buf, n), FAIL_SEG_VADDR);

	/* --- segment overruns top of user window -------- */
	n = build_good_elf(buf, CODE_LO);
	{
		struct elf32_phdr *ph = (struct elf32_phdr *)(buf + 64);
		ph->p_vaddr = CODE_HI - 512;
		ph->p_memsz = 4096;          /* would land past CODE_HI */
	}
	failures += expect_int("seg memsz overruns CODE_HI -> FAIL_SEG_MEMSZ",
	                       validate(buf, n), FAIL_SEG_MEMSZ);

	/* --- entry not in any loaded segment ------------- */
	n = build_good_elf(buf, CODE_LO);
	{
		/* Move the only PT_LOAD past entry, so entry doesn't belong
		 * to any segment loaded into memory. */
		struct elf32_phdr *ph = (struct elf32_phdr *)(buf + 64);
		ph->p_vaddr = CODE_LO + 0x4000;
		ph->p_memsz = 0x1000;
		/* entry still at CODE_LO + 0 -> orphan */
	}
	failures += expect_int("entry not in any seg -> FAIL_ENTRY_ORPH",
	                       validate(buf, n), FAIL_ENTRY_ORPH);

	/* --- segments with memsz=0 ignored (no false-positive) ---- */
	n = build_good_elf(buf, CODE_LO);
	{
		struct elf32_phdr *ph = (struct elf32_phdr *)(buf + 64);
		/* keep the only PT_LOAD valid, but set filesz=memsz=0;
		 * loader should still find entry in this segment via the
		 * 0-length check upgrade? actually a memsz=0 seg is skipped,
		 * so entry becomes orphan. We assert orphan to lock the
		 * 'skip memsz=0' behavior. */
		ph->p_memsz  = 0;
		ph->p_filesz = 0;
	}
	failures += expect_int("memsz=0 seg skipped -> FAIL_ENTRY_ORPH",
	                       validate(buf, n), FAIL_ENTRY_ORPH);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
