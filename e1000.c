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

#define E1000_CTRL_RST  0x04000000u   /* chip reset (self-clearing)  */

/* RCTL (receive control) bits. */
#define E1000_RCTL_EN          0x00000002u   /* receiver enable          */
#define E1000_RCTL_SBP         0x00000004u   /* store bad packets        */
#define E1000_RCTL_UPE         0x00000008u   /* unicast promiscuous      */
#define E1000_RCTL_MPE         0x00000010u   /* multicast promiscuous    */
#define E1000_RCTL_BAM         0x00008000u   /* broadcast accept         */
#define E1000_RCTL_BSIZE_2048  0x00000000u   /* 2 KiB receive buffer     */
#define E1000_RCTL_SECRC       0x04000000u   /* strip CRC from packets   */

/* RX descriptor status bits. */
#define E1000_RXD_STAT_DD      0x01u         /* descriptor done          */
#define E1000_RXD_STAT_EOP     0x02u         /* end of packet            */

#define RX_RING_COUNT  32
#define RX_BUF_SIZE    2048

struct rx_desc {
	unsigned long long addr;
	unsigned short     length;
	unsigned short     checksum;
	unsigned char      status;
	unsigned char      errors;
	unsigned short     special;
} __attribute__((packed));

static struct rx_desc  rx_ring[RX_RING_COUNT] __attribute__((aligned(16)));
static unsigned char   rx_buffers[RX_RING_COUNT][RX_BUF_SIZE] __attribute__((aligned(16)));
static unsigned int    rx_cursor;          /* next descriptor we'll consume */

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
	setup_rx();
	e1000_initialised = 1;
	log_mac();
	serial_write("e1000: RX ring armed\n");
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

/* Wire each RX descriptor at its dedicated 2 KiB buffer and tell the chip
 * about the ring. Identity-mapped kernel memory means the virtual address
 * of rx_ring / rx_buffers is the physical address the controller will DMA
 * to. RDT is set one slot behind RDH so the device sees a fully-populated
 * ring (32 buffers ready to fill). */
static void setup_rx(void)
{
	for (int i = 0; i < RX_RING_COUNT; i++) {
		rx_ring[i].addr     = (unsigned long long)(unsigned int)rx_buffers[i];
		rx_ring[i].status   = 0;
	}
	rx_cursor = 0;

	e1000_write(E1000_RDBAL, (unsigned int)rx_ring);
	e1000_write(E1000_RDBAH, 0);
	e1000_write(E1000_RDLEN, RX_RING_COUNT * sizeof(struct rx_desc));
	e1000_write(E1000_RDH,   0);
	e1000_write(E1000_RDT,   RX_RING_COUNT - 1);

	/* Zero the multicast table so spurious group MACs don't trigger us. */
	for (int i = 0; i < 128; i++)
		e1000_write(E1000_MTA + i * 4, 0);

	e1000_write(E1000_RCTL,
	            E1000_RCTL_EN  | E1000_RCTL_BAM   |
	            E1000_RCTL_UPE | E1000_RCTL_MPE   |
	            E1000_RCTL_BSIZE_2048 | E1000_RCTL_SECRC);
}

/* Polls the next RX descriptor for a completed packet. Returns the frame
 * length on success (0..len_max) and copies the payload into 'buf'; returns
 * 0 when no packet is ready. Advances RDT so the chip can refill the slot. */
unsigned int e1000_poll_receive(void *buf, unsigned int max_len)
{
	if (!e1000_initialised || !buf || max_len == 0)
		return 0;

	struct rx_desc *d = &rx_ring[rx_cursor];
	if (!(d->status & E1000_RXD_STAT_DD))
		return 0;

	unsigned int len = d->length;
	if (len > max_len)
		len = max_len;

	const unsigned char *src = rx_buffers[rx_cursor];
	unsigned char       *dst = (unsigned char *)buf;
	for (unsigned int i = 0; i < len; i++)
		dst[i] = src[i];

	d->status = 0;
	e1000_write(E1000_RDT, rx_cursor);
	rx_cursor = (rx_cursor + 1) % RX_RING_COUNT;
	return len;
}
