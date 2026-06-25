/* Minimal ATA PIO disk reader (primary bus, master, LBA28, polling). */
#ifndef ATA_H
#define ATA_H

/* Both return 0 on success, -1 on a reported drive error or a timeout
 * (instead of hanging forever in a busy-loop on a dead/faulty drive). */
int ata_read(unsigned int lba, unsigned int count, void *buffer);
int ata_write(unsigned int lba, unsigned int count, const void *buffer);

/* IDENTIFY DEVICE: fills model_out (caller-provided, at least 41 bytes) with
 * the drive's model string (trimmed of trailing padding spaces, NUL
 * terminated) and *sectors_out with the LBA28 total sector count. Returns 0
 * on success, -1 on error/timeout. */
int ata_identify(char *model_out, unsigned int *sectors_out);

#endif
