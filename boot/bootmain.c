/* StinkOS boot stage 2: ELF-aware kernel loader.
 *
 *   ============================================================
 *   STATUS: NOT YET WIRED INTO THE BUILD (TODO §13, step 2 of 3)
 *   ============================================================
 *
 * What this file is:
 *   The C half of the bootloader. Once boot.s has us in 32-bit
 *   protected mode with a flat GDT, it will `call bootmain()` (this
 *   function). bootmain reads the kernel ELF image off the boot
 *   disk via ATA PIO (one sector at a time, no driver, no IRQs,
 *   no DMA), walks the PT_LOAD program headers, places each segment
 *   at the physical address the linker chose, zero-fills the .bss
 *   tail of each segment, and jumps to elf->e_entry.
 *
 * What's missing before this runs:
 *   1. Makefile rules to compile boot/bootmain.c + boot/elf32.h
 *      into boot/bootmain.o and link it into the boot image.
 *   2. boot/boot.s must be modified so that after it jumps into
 *      protected mode, it `call`s bootmain instead of running its
 *      own bss-init + `call kmain` path. The current pm_entry
 *      stub becomes unnecessary.
 *   3. The kernel image must ship as ELF on disk (kernel.elf, the
 *      diagnostic artifact already produced by the Makefile after
 *      §13 step 1) and live at LBA KERNEL_LBA onward.
 *
 * Why the existing flat-binary loader is being replaced:
 *   The flat path requires the bootloader to know the kernel size
 *   ahead of time (`KSECTORS = 127` in boot.s), which caps the
 *   kernel at ~64 KiB and turns every kernel growth event into a
 *   bootloader edit + on-disk layout change. The ELF path reads
 *   exactly what the program headers describe -- the kernel can
 *   grow without touching the boot sector. xv6 has used this since
 *   ~2006; see osdev-refs/xv6-public/bootmain.c for the pattern.
 *
 * Things to verify when this gets wired (TODO §13 step 3):
 *   - Reading from primary IDE master via PIO works the same way
 *     the kernel's ata.c already does it (same ports, same status
 *     wait). Hardware that uses secondary bus or AHCI is out of
 *     scope -- StinkOS doesn't boot from those today either.
 *   - The kernel ELF must be loaded at a physical address that
 *     does NOT overlap this bootmain code area (we run at 0x7C00..
 *     somewhere below the kernel). xv6 puts the kernel at 0x100000
 *     (1 MiB), which is safe; we should do the same.
 *   - boot.s must zero the destination .bss range for ANY PT_LOAD
 *     where p_memsz > p_filesz. That's exactly what the stosb call
 *     below does.
 */

#include "elf32.h"

#define SECTSIZE       512u
#define KERNEL_LBA     1u           /* LBA where the kernel ELF starts on disk */
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
