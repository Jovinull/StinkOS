/* Host-side test for the PT_LOAD walk in boot/bootmain.c.
 *
 * bootmain runs in real hardware during the boot transition, where any
 * bug surfaces as "system hangs on a black screen with no log line."
 * Hard to debug. So we exercise the same placement algorithm against an
 * in-memory ELF here, with disk I/O replaced by memcpy from a fake
 * disk buffer to a fake physical RAM. This catches regressions in:
 *
 *   - non-PT_LOAD program-header entries (PT_NOTE, PT_DYNAMIC) being
 *     skipped instead of loaded
 *   - segment content landing at the wrong physical address
 *   - the .bss tail (memsz > filesz) being left uninitialised
 *
 * It does NOT exercise the ATA sector-rounding read_seg() does (that
 * is purely an I/O concern; kernel/sys/elf.c's own loader has a similar
 * trick and is covered by test_elf_loader.c).
 */
#include <stdio.h>
#include <string.h>

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

#define PT_LOAD 1
#define PT_NOTE 4

/* Fake disk: holds the kernel ELF image bootmain would read sector by
 * sector. Capacity is irrelevant for the walk test; pick something
 * large enough for header + a few segments. */
static unsigned char fake_disk[16 * 1024];

/* Fake "low physical RAM" the bootloader writes PT_LOAD segments into.
 * Indexed by physical address; entries start zeroed (mimics post-BIOS
 * RAM, which bootmain assumes is junk and overwrites). */
static unsigned char ram[64 * 1024];

/* Mirror of bootmain's read_seg minus the sector-rounding I/O concerns.
 * The bootloader reads from disk; here we memcpy from fake_disk. */
static void mock_read_seg(unsigned char *pa, unsigned int count, unsigned int offset)
{
	memcpy(pa, fake_disk + offset, count);
}

/* Mirror of bootmain's PT_LOAD walk. Should stay structurally identical
 * to boot/bootmain.c -- if you change one, change the other. */
static void walk_phs(struct elf32_ehdr *eh)
{
	struct elf32_phdr *ph  =
		(struct elf32_phdr *)((unsigned char *)eh + eh->e_phoff);
	struct elf32_phdr *eph = ph + eh->e_phnum;

	for (; ph < eph; ph++) {
		if (ph->p_type != PT_LOAD)
			continue;
		unsigned char *pa = ram + ph->p_paddr;
		mock_read_seg(pa, ph->p_filesz, ph->p_offset);
		if (ph->p_memsz > ph->p_filesz)
			memset(pa + ph->p_filesz, 0, ph->p_memsz - ph->p_filesz);
	}
}

static int failures;

static void expect_eq_bytes(const char *what, const unsigned char *got,
                            const unsigned char *want, unsigned int n)
{
	if (memcmp(got, want, n) != 0) {
		failures++;
		printf("FAIL %s: byte mismatch in %u bytes\n", what, n);
	} else {
		printf("ok   %s\n", what);
	}
}

static void expect_all_zero(const char *what, const unsigned char *got, unsigned int n)
{
	for (unsigned int i = 0; i < n; i++) {
		if (got[i] != 0) {
			failures++;
			printf("FAIL %s: non-zero byte at offset %u (got 0x%02x)\n",
			       what, i, got[i]);
			return;
		}
	}
	printf("ok   %s\n", what);
}

int main(void)
{
	memset(fake_disk, 0, sizeof fake_disk);
	memset(ram, 0xAA, sizeof ram);   /* pre-poison to detect missed writes */

	/* Build the fake ELF inside fake_disk:
	 *   offset 0     -> ehdr (52 bytes)
	 *   offset 52    -> 3 phdrs (96 bytes total)
	 *   offset 0x200 -> segment 1 content (0x100 bytes, "AB" repeated)
	 *   offset 0x400 -> segment 2 content (0x40 bytes, "CD" repeated; memsz larger)
	 *   (PT_NOTE entry has no segment payload -- non-PT_LOAD is skipped)
	 */
	struct elf32_ehdr *eh = (struct elf32_ehdr *)fake_disk;
	eh->e_ident[0] = 0x7F;
	eh->e_ident[1] = 'E';
	eh->e_ident[2] = 'L';
	eh->e_ident[3] = 'F';
	eh->e_phoff    = 52;
	eh->e_phnum    = 3;
	eh->e_entry    = 0x1000;

	struct elf32_phdr *phs = (struct elf32_phdr *)(fake_disk + 52);

	/* PT_LOAD #1: filesz == memsz, no bss tail. */
	phs[0].p_type   = PT_LOAD;
	phs[0].p_offset = 0x200;
	phs[0].p_paddr  = 0x1000;
	phs[0].p_filesz = 0x100;
	phs[0].p_memsz  = 0x100;
	for (unsigned int i = 0; i < 0x100; i++)
		fake_disk[0x200 + i] = (i & 1) ? 'B' : 'A';

	/* PT_NOTE: must be skipped. We park "garbage" at its paddr to prove
	 * the walk never touches it. */
	phs[1].p_type   = PT_NOTE;
	phs[1].p_offset = 0x300;
	phs[1].p_paddr  = 0x5000;       /* if loaded, would clobber ram[0x5000] */
	phs[1].p_filesz = 0x20;
	phs[1].p_memsz  = 0x20;

	/* PT_LOAD #2: memsz > filesz -- the trailing region is .bss and
	 * MUST be zero-filled by the loader, not left as the 0xAA poison. */
	phs[2].p_type   = PT_LOAD;
	phs[2].p_offset = 0x400;
	phs[2].p_paddr  = 0x2000;
	phs[2].p_filesz = 0x40;
	phs[2].p_memsz  = 0x200;        /* 0x1C0 bytes of bss tail */
	for (unsigned int i = 0; i < 0x40; i++)
		fake_disk[0x400 + i] = (i & 1) ? 'D' : 'C';

	walk_phs(eh);

	/* Segment 1 content placed exactly. */
	expect_eq_bytes("PT_LOAD #1 placed at p_paddr",
	                ram + 0x1000, fake_disk + 0x200, 0x100);

	/* Segment 2 file content placed exactly. */
	expect_eq_bytes("PT_LOAD #2 file content placed at p_paddr",
	                ram + 0x2000, fake_disk + 0x400, 0x40);

	/* Segment 2 bss tail zero-filled. */
	expect_all_zero("PT_LOAD #2 bss tail (memsz - filesz) zeroed",
	                ram + 0x2000 + 0x40, 0x200 - 0x40);

	/* PT_NOTE region untouched (still poison 0xAA). */
	if (ram[0x5000] != 0xAA || ram[0x501F] != 0xAA) {
		failures++;
		printf("FAIL PT_NOTE region was loaded (should have been skipped)\n");
	} else {
		printf("ok   PT_NOTE skipped (poison intact at p_paddr)\n");
	}

	/* Nothing outside the two PT_LOAD destinations should have changed. */
	if (ram[0x0FFF] != 0xAA || ram[0x1100] != 0xAA ||
	    ram[0x1FFF] != 0xAA || ram[0x2200] != 0xAA) {
		failures++;
		printf("FAIL writes spilled outside PT_LOAD destination ranges\n");
	} else {
		printf("ok   no writes outside PT_LOAD destination ranges\n");
	}

	if (failures) {
		printf("\n%d failure(s)\n", failures);
		return 1;
	}
	printf("\nall checks passed\n");
	return 0;
}
