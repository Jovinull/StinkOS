/* ATA PIO + Bus Master DMA driver (LBA28). Supports the four standard ISA IDE
 * slots: primary master/slave + secondary master/slave. The plain ata_*
 * functions still address drive 0 (primary master) for compatibility with
 * the existing kernel + apps; new code goes through ata_drive_* to pick a
 * specific drive.
 *
 * Call ata_dma_init() once after pci_scan() to probe for a PIIX IDE controller
 * and enable Bus Master DMA. If the probe fails the driver silently falls back
 * to PIO for all subsequent transfers. */
#ifndef ATA_H
#define ATA_H

/* Probe for a PIIX3/PIIX4 IDE controller and enable Bus Master DMA.
 * Safe to call even when no PIIX device is present; falls back to PIO. */
void ata_dma_init(void);

/* Drive index: 0..3 maps to (primary|secondary) x (master|slave). */
int ata_drive_read    (int drive, unsigned int lba, unsigned int count, void *buffer);
int ata_drive_write   (int drive, unsigned int lba, unsigned int count, const void *buffer);
int ata_drive_identify(int drive, char *model_out, unsigned int *sectors_out);

/* Drive 0 shortcuts. Both return 0 on success, -1 on a reported drive
 * error or a timeout (instead of hanging forever on a dead/faulty drive). */
int ata_read(unsigned int lba, unsigned int count, void *buffer);
int ata_write(unsigned int lba, unsigned int count, const void *buffer);

/* IDENTIFY DEVICE on drive 0. model_out must be at least 41 bytes. */
int ata_identify(char *model_out, unsigned int *sectors_out);

#endif
