/* Minimal ATA PIO disk reader (primary bus, master, LBA28, polling). */
#ifndef ATA_H
#define ATA_H

void ata_read(unsigned int lba, unsigned int count, void *buffer);

#endif
