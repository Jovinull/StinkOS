/* ATA PIO + Bus Master DMA disk driver (LBA28, drive-aware).
 *
 * Drive index layout:
 *   0 = primary master   (I/O base 0x1F0, drive byte 0xE0)
 *   1 = primary slave    (I/O base 0x1F0, drive byte 0xF0)
 *   2 = secondary master (I/O base 0x170, drive byte 0xE0)
 *   3 = secondary slave  (I/O base 0x170, drive byte 0xF0)
 *
 * When DMA is available (ata_dma_init() succeeded), ata_drive_read/write use
 * the Bus Master IDE engine for transfers up to 128 sectors, falling back to
 * PIO for larger requests or if the PIIX IDE controller was not found.
 *
 * The plain ata_* functions still address drive 0 for backward compatibility
 * with all existing kernel and app callers. */
#include "ata.h"
#include "pci.h"
#include "serial.h"
#include "io.h"

/* ---- ATA register offsets (from channel base) ---- */
#define ATA_PRIMARY_BASE   0x1F0
#define ATA_SECONDARY_BASE 0x170

#define ATA_REG_DATA       0x00
#define ATA_REG_SECCOUNT   0x02
#define ATA_REG_LBA_LO     0x03
#define ATA_REG_LBA_MID    0x04
#define ATA_REG_LBA_HI     0x05
#define ATA_REG_DRIVE      0x06
#define ATA_REG_CMD        0x07     /* write: command  / read: status */

#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_BSY 0x80

#define CMD_READ_SECTORS  0x20
#define CMD_WRITE_SECTORS 0x30
#define CMD_FLUSH_CACHE   0xE7
#define CMD_IDENTIFY      0xEC
#define CMD_READ_DMA      0xC8
#define CMD_WRITE_DMA     0xCA

#define ATA_TIMEOUT_SPINS 1000000

/* ---- Bus Master IDE (BMIDE) register offsets from channel base ---- */
#define BMIDE_CMD    0x00   /* bit 0 = start/stop, bit 3 = read(1)/write(0) */
#define BMIDE_STATUS 0x02   /* bit 1 = error, bit 2 = IRQ (write-1-to-clear) */
#define BMIDE_PRDT   0x04   /* 32-bit physical address of the PRDT */

/* Physical Region Descriptor Table entry. Each entry describes one
 * contiguous physical buffer that the DMA engine should transfer.
 * 4-byte aligned; must not cross a 64 KiB physical boundary. */
struct prdt_entry {
    unsigned int  phys;          /* physical base address of the buffer */
    unsigned short bytes;        /* byte count; 0 == 64 KiB */
    unsigned short eot;          /* bit 15 set = end of table */
} __attribute__((packed));

/* Static PRDT: one entry is sufficient for a single DMA region. */
static struct prdt_entry dma_prdt[1] __attribute__((aligned(4)));

/* 64 KiB bounce buffer, aligned to its own size so it can never cross a
 * 64 KiB physical page boundary (a hard constraint for Bus Master DMA). The
 * buffer lives in BSS and is NOT included in the on-disk kernel image. */
static unsigned char dma_buf[65536] __attribute__((aligned(65536)));

/* Base I/O port of the Bus Master IDE registers for the primary channel.
 * Zero means DMA is unavailable; the driver falls back to PIO. */
static unsigned short bmide_base;

/* ---- helpers ---- */

static unsigned short ata_base_for(int drive)
{
    return (drive < 2) ? ATA_PRIMARY_BASE : ATA_SECONDARY_BASE;
}

static unsigned char ata_drive_byte(int drive)
{
    return (drive & 1) ? 0xF0u : 0xE0u;
}

static int ata_wait_ready_base(unsigned short base)
{
    unsigned int spins = 0;
    while ((inb(base + ATA_REG_CMD) & ST_BSY) && spins < ATA_TIMEOUT_SPINS)
        spins++;
    return (spins < ATA_TIMEOUT_SPINS) ? 0 : -1;
}

static int ata_poll_base(unsigned short base)
{
    if (ata_wait_ready_base(base) != 0)
        return -1;

    unsigned int spins = 0;
    for (;;) {
        unsigned char status = inb(base + ATA_REG_CMD);
        if (status & ST_ERR)
            return -1;
        if (status & ST_DRQ)
            return 0;
        if (++spins >= ATA_TIMEOUT_SPINS)
            return -1;
    }
}

static int ata_select_drive(int drive, unsigned int lba,
                            unsigned int count, unsigned char cmd)
{
    unsigned short base  = ata_base_for(drive);
    unsigned char  dbyte = ata_drive_byte(drive);

    if (ata_wait_ready_base(base) != 0)
        return -1;

    outb(base + ATA_REG_DRIVE,    dbyte | ((lba >> 24) & 0x0Fu));
    outb(base + ATA_REG_SECCOUNT, (unsigned char)count);
    outb(base + ATA_REG_LBA_LO,   lba & 0xFFu);
    outb(base + ATA_REG_LBA_MID, (lba >> 8) & 0xFFu);
    outb(base + ATA_REG_LBA_HI,  (lba >> 16) & 0xFFu);
    outb(base + ATA_REG_CMD,     cmd);
    return 0;
}

static void dma_copy(void *dst, const void *src, unsigned int n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--)
        *d++ = *s++;
}

/* ---- Bus Master DMA initialisation ---- */

void ata_dma_init(void)
{
    /* QEMU emulates Intel PIIX3 (0x8086:0x7010) or PIIX4 (0x8086:0x7111).
     * Both expose Bus Master IDE through PCI BAR4. */
    struct pci_addr ide;
    if (!pci_find(0x8086, 0x7010, &ide) &&
        !pci_find(0x8086, 0x7111, &ide)) {
        serial_write("ata: no PIIX IDE found, using PIO\n");
        return;
    }

    /* Enable bus mastering so the controller may initiate DMA cycles. */
    unsigned short cmd = pci_read16(ide, PCI_COMMAND);
    if (!(cmd & PCI_CMD_BUSMASTER))
        pci_write32(ide, PCI_COMMAND, (unsigned int)cmd | PCI_CMD_BUSMASTER);

    /* BAR4: I/O space (bit 0 = 1). Mask the indicator bit. */
    unsigned int bar4 = pci_read32(ide, PCI_BAR4);
    if (!(bar4 & 1u)) {
        serial_write("ata: BMIDE BAR4 is MMIO, using PIO\n");
        return;
    }
    bmide_base = (unsigned short)(bar4 & 0xFFFCu);
    serial_write("ata: Bus Master IDE enabled\n");
}

/* ---- DMA transfer (read or write), primary/secondary channel ---- */

static int ata_dma_transfer(int drive, unsigned int lba, unsigned int count,
                            void *buffer, int is_write)
{
    unsigned short bm  = bmide_base + (unsigned short)((drive < 2) ? 0 : 8);
    unsigned short aio = ata_base_for(drive);

    unsigned int bytes = count * 512u;

    if (is_write)
        dma_copy(dma_buf, buffer, bytes);

    /* Build a single-entry PRDT covering the entire transfer. */
    dma_prdt[0].phys  = (unsigned int)(unsigned long)dma_buf;
    dma_prdt[0].bytes = (unsigned short)(bytes < 65536u ? bytes : 0u); /* 0 == 64 KiB */
    dma_prdt[0].eot   = 0x8000u;                                       /* end of table */

    /* Initialise Bus Master registers. */
    outb(bm + BMIDE_CMD,    0);                           /* stop any prior DMA */
    outl(bm + BMIDE_PRDT,   (unsigned int)(unsigned long)dma_prdt);
    outb(bm + BMIDE_STATUS, inb(bm + BMIDE_STATUS) | 0x06u); /* clear err+IRQ */
    outb(bm + BMIDE_CMD,    is_write ? 0x00u : 0x08u);   /* set direction */

    /* Program the ATA drive (LBA28, drive select). */
    if (ata_wait_ready_base(aio) != 0)
        return -1;
    outb(aio + ATA_REG_DRIVE,    ata_drive_byte(drive) | ((lba >> 24) & 0x0Fu));
    outb(aio + ATA_REG_SECCOUNT, (unsigned char)count);
    outb(aio + ATA_REG_LBA_LO,   lba & 0xFFu);
    outb(aio + ATA_REG_LBA_MID, (lba >> 8) & 0xFFu);
    outb(aio + ATA_REG_LBA_HI,  (lba >> 16) & 0xFFu);
    outb(aio + ATA_REG_CMD,      is_write ? CMD_WRITE_DMA : CMD_READ_DMA);

    /* Start DMA (bit 0 = start; bit 3: 0 = write to disk, 1 = read from disk). */
    outb(bm + BMIDE_CMD, is_write ? 0x01u : 0x09u);

    /* Poll Bus Master status until IRQ (bit 2) or error/timeout. */
    unsigned int spins = 0;
    for (;;) {
        unsigned char st = inb(bm + BMIDE_STATUS);
        if (st & 0x04u) break;              /* IRQ set: transfer complete */
        if (st & 0x02u) {                   /* error bit */
            outb(bm + BMIDE_CMD, 0);
            return -1;
        }
        if (++spins >= ATA_TIMEOUT_SPINS) {
            outb(bm + BMIDE_CMD, 0);
            return -1;
        }
    }

    outb(bm + BMIDE_CMD,    0);                           /* stop DMA */
    outb(bm + BMIDE_STATUS, inb(bm + BMIDE_STATUS) | 0x06u); /* clear bits */

    if (!is_write)
        dma_copy(buffer, dma_buf, bytes);

    return 0;
}

/* ---- Public drive-aware API ---- */

int ata_drive_read(int drive, unsigned int lba, unsigned int count, void *buffer)
{
    if (drive < 0 || drive > 3)
        return -1;

    /* Use DMA for primary channel when available, up to 128 sectors (64 KiB). */
    if (bmide_base && drive < 2 && count > 0 && count <= 128)
        return ata_dma_transfer(drive, lba, count, buffer, 0);

    /* PIO fallback. */
    unsigned short  base = ata_base_for(drive);
    unsigned short *buf  = (unsigned short *)buffer;
    if (ata_select_drive(drive, lba, count, CMD_READ_SECTORS) != 0)
        return -1;
    for (unsigned int s = 0; s < count; s++) {
        if (ata_poll_base(base) != 0)
            return -1;
        for (int i = 0; i < 256; i++)
            buf[i] = inw(base + ATA_REG_DATA);
        buf += 256;
    }
    return 0;
}

int ata_drive_write(int drive, unsigned int lba, unsigned int count, const void *buffer)
{
    if (drive < 0 || drive > 3)
        return -1;

    /* DMA write: copy to bounce buffer first, then DMA, then flush cache. */
    if (bmide_base && drive < 2 && count > 0 && count <= 128) {
        if (ata_dma_transfer(drive, lba, count, (void *)buffer, 1) != 0)
            return -1;
        unsigned short base = ata_base_for(drive);
        outb(base + ATA_REG_CMD, CMD_FLUSH_CACHE);
        return ata_wait_ready_base(base);
    }

    /* PIO fallback. */
    unsigned short        base = ata_base_for(drive);
    const unsigned short *buf  = (const unsigned short *)buffer;
    if (ata_select_drive(drive, lba, count, CMD_WRITE_SECTORS) != 0)
        return -1;
    for (unsigned int s = 0; s < count; s++) {
        if (ata_poll_base(base) != 0)
            return -1;
        for (int i = 0; i < 256; i++)
            outw(base + ATA_REG_DATA, buf[i]);
        buf += 256;
    }
    outb(base + ATA_REG_CMD, CMD_FLUSH_CACHE);
    return ata_wait_ready_base(base);
}

int ata_drive_identify(int drive, char *model_out, unsigned int *sectors_out)
{
    if (drive < 0 || drive > 3)
        return -1;
    unsigned short base = ata_base_for(drive);

    if (ata_select_drive(drive, 0, 0, CMD_IDENTIFY) != 0)
        return -1;
    if (ata_poll_base(base) != 0)
        return -1;

    unsigned short data[256];
    for (int i = 0; i < 256; i++)
        data[i] = inw(base + ATA_REG_DATA);

    for (int w = 0; w < 20; w++) {
        unsigned short word = data[27 + w];
        model_out[w * 2]     = (char)(word >> 8);
        model_out[w * 2 + 1] = (char)(word & 0xFF);
    }
    model_out[40] = '\0';
    for (int i = 39; i >= 0 && model_out[i] == ' '; i--)
        model_out[i] = '\0';

    *sectors_out = ((unsigned int)data[61] << 16) | data[60];
    return 0;
}

/* ---- Drive 0 compatibility shims ---- */

int ata_read(unsigned int lba, unsigned int count, void *buffer)
{
    return ata_drive_read(0, lba, count, buffer);
}

int ata_write(unsigned int lba, unsigned int count, const void *buffer)
{
    return ata_drive_write(0, lba, count, buffer);
}

int ata_identify(char *model_out, unsigned int *sectors_out)
{
    return ata_drive_identify(0, model_out, sectors_out);
}
