/* Intel 82540EM (e1000) gigabit NIC driver. QEMU emulates this card by
 * default when given `-netdev user,id=net0 -device e1000,netdev=net0`, so
 * it's the practical target for any hobby OS that wants a working NIC in a
 * VM. The chip exposes a 128 KiB MMIO region (BAR0) with separate RX and TX
 * descriptor rings, plus per-ring head/tail pointers.
 *
 * This header is intentionally driver-side only; the network stack on top
 * (Ethernet -> IP -> TCP/UDP) calls e1000_send_frame / consumes
 * e1000_poll_receive without knowing about MMIO or descriptor layouts. */
#ifndef E1000_H
#define E1000_H

#define E1000_VENDOR_ID  0x8086u
#define E1000_DEVICE_ID  0x100Eu     /* QEMU's default 82540EM */

/* Initialise the e1000: locate the device on PCI, enable bus-mastering,
 * map MMIO, reset the controller and pull the MAC out of EEPROM (or the
 * RAH/RAL registers, which QEMU initialises directly). Returns 1 on success,
 * 0 if no e1000 is present. Logs the MAC on the serial console either way. */
int  e1000_init(void);

/* Returns 1 if e1000_init succeeded. */
int  e1000_present(void);

/* Copies the 6-byte MAC into 'out' (must be at least 6 bytes). Returns 0 on
 * success, -1 if the device isn't present. */
int  e1000_get_mac(unsigned char *out);

#endif
