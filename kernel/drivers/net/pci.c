/* PCI configuration-space access via the legacy port mechanism (#1).
 *
 * Each access takes two I/O cycles: write a 32-bit "config address" to
 * 0xCF8, then read or write 32-bit data from/to 0xCFC. The address word
 * has a fixed enable bit (0x80000000), the (bus, dev, fn) triple, and the
 * 6 high bits of the register offset (the chipset always serves dword-
 * aligned reads). */
#include "pci.h"
#include "serial.h"
#include "io.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static unsigned int make_address(struct pci_addr a, unsigned char reg)
{
	return 0x80000000u |
	       ((unsigned int)a.bus << 16) |
	       ((unsigned int)a.dev << 11) |
	       ((unsigned int)a.fn  <<  8) |
	       (reg & 0xFCu);
}

unsigned int pci_read32(struct pci_addr a, unsigned char reg)
{
	outl(PCI_CONFIG_ADDRESS, make_address(a, reg));
	return inl(PCI_CONFIG_DATA);
}

void pci_write32(struct pci_addr a, unsigned char reg, unsigned int value)
{
	outl(PCI_CONFIG_ADDRESS, make_address(a, reg));
	outl(PCI_CONFIG_DATA, value);
}

unsigned short pci_read16(struct pci_addr a, unsigned char reg)
{
	unsigned int w = pci_read32(a, reg & 0xFCu);
	return (unsigned short)((w >> ((reg & 2) * 8)) & 0xFFFFu);
}

unsigned char pci_read8(struct pci_addr a, unsigned char reg)
{
	unsigned int w = pci_read32(a, reg & 0xFCu);
	return (unsigned char)((w >> ((reg & 3) * 8)) & 0xFFu);
}

/* Vendor ID 0xFFFF (all ones) is the standard "no device" sentinel: an
 * empty PCI slot responds with that pattern from address 0. */
static int slot_present(struct pci_addr a)
{
	return pci_read16(a, PCI_VENDOR_ID) != 0xFFFFu;
}

void pci_scan(void)
{
	serial_write("pci: scanning bus...\n");
	for (unsigned int bus = 0; bus < 256; bus++) {
		for (unsigned int dev = 0; dev < 32; dev++) {
			struct pci_addr a = { (unsigned char)bus,
			                       (unsigned char)dev, 0 };
			if (!slot_present(a))
				continue;

			unsigned short vendor = pci_read16(a, PCI_VENDOR_ID);
			unsigned short device = pci_read16(a, PCI_DEVICE_ID);
			unsigned char  cls    = pci_read8(a, PCI_CLASS);

			serial_write("pci: ");
			serial_write_dec(bus);
			serial_putc(':');
			serial_write_dec(dev);
			serial_write(" vendor=0x");
			serial_write_hex(vendor);
			serial_write(" device=0x");
			serial_write_hex(device);
			serial_write(" class=0x");
			serial_write_hex(cls);
			serial_putc('\n');
		}
	}
}

int pci_find(unsigned short vendor, unsigned short device, struct pci_addr *out)
{
	for (unsigned int bus = 0; bus < 256; bus++) {
		for (unsigned int dev = 0; dev < 32; dev++) {
			struct pci_addr a = { (unsigned char)bus,
			                       (unsigned char)dev, 0 };
			if (!slot_present(a))
				continue;
			if (pci_read16(a, PCI_VENDOR_ID) == vendor &&
			    pci_read16(a, PCI_DEVICE_ID) == device) {
				if (out)
					*out = a;
				return 1;
			}
		}
	}
	return 0;
}
