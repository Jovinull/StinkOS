/* Minimal ATA PIO disk reader (primary bus, master, LBA28, polling). */
#ifndef ATA_H
#define ATA_H

/* Both return 0 on success, -1 on a reported drive error or a timeout
 * (instead of hanging forever in a busy-loop on a dead/faulty drive). */
int ata_read(unsigned int lba, unsigned int count, void *buffer);
int ata_write(unsigned int lba, unsigned int count, const void *buffer);

#endif
