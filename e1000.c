/* Intel 82540EM (e1000) gigabit NIC driver. Boot-time scaffold: probe via
 * PCI, claim BAR0, enable bus mastering, issue a chip reset, and read the
 * MAC out of the receive-address-low/high registers. RX/TX descriptor rings
 * land in a follow-up commit; this file gets us to "MAC visible on serial",
 * which is the first ground-truth that the rest of the stack can rely on.
 */
#include "e1000.h"
#include "pci.h"
#include "serial.h"

/* Register offsets from MMIO base (BAR0). The Intel SDM lists hundreds; we
 * only need a handful at this layer. */
#define E1000_CTRL      0x0000     /* device control */
#define E1000_STATUS    0x0008
#define E1000_EECD      0x0010
#define E1000_EERD      0x0014     /* EEPROM read register */
#define E1000_CTRL_EXT  0x0018
#define E1000_ICR       0x00C0     /* interrupt cause read (clears on read) */
#define E1000_IMS       0x00D0     /* interrupt mask set */
#define E1000_IMC       0x00D8     /* interrupt mask clear */
#define E1000_RCTL      0x0100
#define E1000_TCTL      0x0400
#define E1000_RDBAL     0x2800
#define E1000_RDBAH     0x2804
#define E1000_RDLEN     0x2808
#define E1000_RDH       0x2810
#define E1000_RDT       0x2818
#define E1000_TDBAL     0x3800
#define E1000_TDBAH     0x3804
#define E1000_TDLEN     0x3808
#define E1000_TDH       0x3810
#define E1000_TDT       0x3818
#define E1000_MTA       0x5200     /* multicast table array, 128 dwords */
#define E1000_RAL0      0x5400     /* receive address low (entry 0)  */
#define E1000_RAH0      0x5404     /* receive address high (entry 0) */

#define E1000_CTRL_RST  0x04000000u   /* chip reset (self-clearing) */

static volatile unsigned int *e1000_mmio;
static int                    e1000_initialised;
static unsigned char          e1000_mac[6];

static unsigned int e1000_read(unsigned int reg)
{
	return e1000_mmio[reg / 4];
}

static void e1000_write(unsigned int reg, unsigned int value)
{
	e1000_mmio[reg / 4] = value;
}

/* QEMU's e1000 model fills the RAL/RAH registers with the configured MAC at
 * power-on, so we can skip the (more complex) EEPROM read path entirely for
 * the typical hobby setup. */
static void read_mac_from_rar(void)
{
	unsigned int low  = e1000_read(E1000_RAL0);
	unsigned int high = e1000_read(E1000_RAH0);
	e1000_mac[0] = (unsigned char)(low        & 0xFF);
	e1000_mac[1] = (unsigned char)((low >> 8) & 0xFF);
	e1000_mac[2] = (unsigned char)((low >> 16) & 0xFF);
	e1000_mac[3] = (unsigned char)((low >> 24) & 0xFF);
	e1000_mac[4] = (unsigned char)(high       & 0xFF);
	e1000_mac[5] = (unsigned char)((high >> 8) & 0xFF);
}

static void log_mac(void)
{
	serial_write("e1000: mac ");
	for (int i = 0; i < 6; i++) {
		if (i)
			serial_putc(':');
		serial_write_hex(e1000_mac[i]);
	}
	serial_putc('\n');
}

int e1000_init(void)
{
	struct pci_addr a;
	if (!pci_find(E1000_VENDOR_ID, E1000_DEVICE_ID, &a)) {
		serial_write("e1000: no device "
		             "(start QEMU with -device e1000)\n");
		return 0;
	}

	/* BAR0 lower bit 0 = I/O space flag; mask off the low 4 bits to get
	 * the MMIO base. The kernel identity-maps low physical memory, and
	 * QEMU places the e1000 BAR in the conventional sub-1 GiB area, so
	 * dereferencing the physical address directly works without extra
	 * page-table fiddling at this stage. */
	unsigned int bar0 = pci_read32(a, PCI_BAR0);
	if (bar0 & 1) {
		serial_write("e1000: BAR0 is I/O space, unsupported\n");
		return 0;
	}
	unsigned int mmio_phys = bar0 & 0xFFFFFFF0u;
	e1000_mmio = (volatile unsigned int *)mmio_phys;

	/* Turn on memory + bus-master bits in the command register, otherwise
	 * the device will not see CPU writes or initiate its own DMA. */
	unsigned int cmd = pci_read32(a, PCI_COMMAND) & 0xFFFFu;
	cmd |= PCI_CMD_MEM | PCI_CMD_BUSMASTER;
	pci_write32(a, PCI_COMMAND, cmd);

	/* Software reset: write CTRL.RST, then poll for it to self-clear. */
	e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | E1000_CTRL_RST);
	for (unsigned int spins = 0; spins < 100000; spins++) {
		if (!(e1000_read(E1000_CTRL) & E1000_CTRL_RST))
			break;
	}

	read_mac_from_rar();
	e1000_initialised = 1;
	log_mac();
	return 1;
}

int e1000_present(void) { return e1000_initialised; }

int e1000_get_mac(unsigned char *out)
{
	if (!e1000_initialised || !out)
		return -1;
	for (int i = 0; i < 6; i++)
		out[i] = e1000_mac[i];
	return 0;
}
