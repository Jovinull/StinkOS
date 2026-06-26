/* PCI configuration-space access (legacy port-based mechanism #1). The PCI
 * bus exposes a 256-byte configuration space per (bus, device, function)
 * tuple, accessed via I/O ports 0xCF8 (address) and 0xCFC (data) in 32-bit
 * aligned reads. This module provides the read/write primitives plus a
 * device-finder for drivers that want a specific vendor/device ID.
 *
 * The kernel-side network and (eventually) AHCI / NVMe drivers all enter
 * through here. Driver code never writes to 0xCF8/0xCFC directly. */
#ifndef PCI_H
#define PCI_H

/* Standard configuration-space register offsets (all in bytes, 32-bit
 * aligned). The reads/writes below mask to dword alignment automatically. */
#define PCI_VENDOR_ID    0x00
#define PCI_DEVICE_ID    0x02
#define PCI_COMMAND      0x04
#define PCI_STATUS       0x06
#define PCI_CLASS        0x0B
#define PCI_HEADER_TYPE  0x0E
#define PCI_BAR0         0x10
#define PCI_BAR1         0x14
#define PCI_BAR2         0x18
#define PCI_BAR3         0x1C
#define PCI_BAR4         0x20
#define PCI_BAR5         0x24
#define PCI_INT_LINE     0x3C

#define PCI_CMD_IO        0x0001    /* respond to I/O accesses        */
#define PCI_CMD_MEM       0x0002    /* respond to MMIO accesses       */
#define PCI_CMD_BUSMASTER 0x0004    /* device may initiate DMA cycles */

/* Address triple that identifies a PCI device. Returned by pci_find. */
struct pci_addr {
	unsigned char bus;
	unsigned char dev;
	unsigned char fn;
};

/* 32-bit aligned config reads/writes. 'reg' is the byte offset; only its
 * upper 6 bits matter for the address packet -- the chipset always returns
 * a 32-bit word, the higher levels mask down to 8/16-bit fields. */
unsigned int   pci_read32(struct pci_addr a, unsigned char reg);
void           pci_write32(struct pci_addr a, unsigned char reg, unsigned int value);
unsigned short pci_read16(struct pci_addr a, unsigned char reg);
unsigned char  pci_read8(struct pci_addr a, unsigned char reg);

/* Walk the entire PCI tree and log every present device on the serial debug
 * console. Called once during kernel boot for visibility; drivers use
 * pci_find for actual lookups. */
void pci_scan(void);

/* Locates the first PCI device with the given vendor+device ID pair. Returns
 * 1 and fills *out on success, 0 if no matching device exists. */
int pci_find(unsigned short vendor, unsigned short device, struct pci_addr *out);

#endif
