/* StinkOS boot stage 2: ELF-aware kernel loader. WIRED.
 *
 * boot/boot.s puts the CPU into 32-bit protected mode with a flat GDT
 * and a stack at STACK_TOP (0x90000), then calls bootmain() here.
 * bootmain reads the kernel ELF off the boot disk starting at
 * KERNEL_LBA via ATA PIO (one sector at a time, no driver, no IRQs,
 * no DMA), walks the PT_LOAD program headers, copies each segment to
 * the physical address the linker chose (kernel.elf is linked at
 * 0x100000), zero-fills the .bss tail of each PT_LOAD where
 * memsz > filesz, and jumps to elf->e_entry (which lands in
 * _kernel_start, see kernel/arch/kentry.s).
 *
 * The bootblock lives at 0x7C00..0x9FFF. The kernel loads at 0x100000+.
 * The two regions never overlap, so the bootblock's own code is safe
 * to keep executing during the PT_LOAD copies; once we jump to the
 * kernel entry, the bootblock memory is dead.
 *
 * Pattern follows xv6's bootmain.c (osdev-refs/xv6-public). Differences:
 *   - bootblock spans multiple sectors instead of fitting in 512 B
 *     (we have VBE setup in boot.s that xv6 lacks); KERNEL_LBA is
 *     therefore pushed past the bootblock area.
 *   - struct elf32_ehdr layout follows the System V ABI strictly
 *     (e_ident[16] then fields), not xv6's "uint magic + uchar elf[12]"
 *     punning. Functionally identical; spec-faithful.
 */

#include "elf32.h"

#define SECTSIZE       512u
#define KERNEL_LBA     16u          /* LBA where kernel.elf starts on disk.
                                       Must match BOOTBLOCK_SECTORS in boot.s
                                       and KERNEL_LBA in Makefile. */
#define SCRATCH_ADDR   0x10000u     /* where we slurp the ELF header + phdrs */

/* ATA primary-bus PIO ports. These match the constants in
 * kernel/drivers/storage/ata.c -- same hardware, smaller surface. */
#define ATA_DATA       0x1F0
#define ATA_COUNT      0x1F2
#define ATA_LBA_LO     0x1F3
#define ATA_LBA_MID    0x1F4
#define ATA_LBA_HI     0x1F5
#define ATA_DRIVE      0x1F6        /* OR with 0xE0 = LBA mode, primary master */
#define ATA_STATUS     0x1F7        /* read = status; write = command */
#define ATA_CMD_READ   0x20

static inline unsigned char inb(unsigned short p)
{
	unsigned char v;
	__asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(p));
	return v;
}

static inline void outb(unsigned short p, unsigned char v)
{
	__asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(p));
}

/* Drain SECTSIZE bytes from the data port in 32-bit chunks. */
static inline void insl(unsigned short p, void *dst, unsigned int cnt32)
{
	__asm__ volatile ("cld; rep insl"
	                  : "=D"(dst), "=c"(cnt32)
	                  : "d"(p), "0"(dst), "1"(cnt32)
	                  : "memory", "cc");
}

static inline void stosb(void *dst, int v, unsigned int cnt)
{
	__asm__ volatile ("cld; rep stosb"
	                  : "=D"(dst), "=c"(cnt)
	                  : "0"(dst), "1"(cnt), "a"(v)
	                  : "memory", "cc");
}

/* Wait until the disk reports ready (BSY clear, DRQ set). */
static void wait_disk(void)
{
	while ((inb(ATA_STATUS) & 0xC0) != 0x40)
		;
}

/* Read exactly one 512-byte sector at LBA `offset` into `dst`. */
static void read_sect(void *dst, unsigned int offset)
{
	wait_disk();
	outb(ATA_COUNT,   1);
	outb(ATA_LBA_LO,  (unsigned char)(offset       & 0xFF));
	outb(ATA_LBA_MID, (unsigned char)((offset >> 8)  & 0xFF));
	outb(ATA_LBA_HI,  (unsigned char)((offset >> 16) & 0xFF));
	outb(ATA_DRIVE,   (unsigned char)(((offset >> 24) & 0x0F) | 0xE0));
	outb(ATA_STATUS,  ATA_CMD_READ);
	wait_disk();
	insl(ATA_DATA, dst, SECTSIZE / 4);
}

/* Read `count` bytes at byte-offset `offset` (from the start of the kernel
 * image, NOT from the start of the disk) into physical address `pa`. May
 * write more than asked because we round down to sector boundaries; the
 * loop loads in increasing order so the overlap always lands on already-
 * correct bytes. */
static void read_seg(unsigned char *pa, unsigned int count, unsigned int offset)
{
	unsigned char *epa = pa + count;

	pa -= offset % SECTSIZE;
	unsigned int lba = (offset / SECTSIZE) + KERNEL_LBA;

	for (; pa < epa; pa += SECTSIZE, lba++)
		read_sect(pa, lba);
}

/* Entry point from boot.s. Loads the kernel ELF and jumps to its entry. */
void bootmain(void)
{
	struct elf32_ehdr *eh = (struct elf32_ehdr *)SCRATCH_ADDR;

	/* Pull the first 4 KiB so the ELF header plus all program headers
	 * are in memory at SCRATCH_ADDR. 4 KiB is plenty for ~120 phdrs. */
	read_seg((unsigned char *)eh, 4096, 0);

	/* Magic check. If this fails we have nothing to do and no way to
	 * report it from here -- spin so the caller can detect the hang
	 * (boot.s has a halt loop after the `call bootmain`). */
	if (!(eh->e_ident[0] == 0x7F &&
	      eh->e_ident[1] == 'E'  &&
	      eh->e_ident[2] == 'L'  &&
	      eh->e_ident[3] == 'F'))
		return;

	/* Walk PT_LOAD program headers and copy each segment into place. */
	struct elf32_phdr *ph  =
	    (struct elf32_phdr *)((unsigned char *)eh + eh->e_phoff);
	struct elf32_phdr *eph = ph + eh->e_phnum;

	for (; ph < eph; ph++) {
		if (ph->p_type != PT_LOAD)
			continue;
		unsigned char *pa = (unsigned char *)ph->p_paddr;
		read_seg(pa, ph->p_filesz, ph->p_offset);
		if (ph->p_memsz > ph->p_filesz)
			stosb(pa + ph->p_filesz, 0,
			      ph->p_memsz - ph->p_filesz);
	}

	/* Transfer to the kernel. Should never return. */
	void (*entry)(void) = (void (*)(void))(unsigned long)eh->e_entry;
	entry();
}
