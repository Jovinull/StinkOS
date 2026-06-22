/* COM1 serial console (38400 8N1) used as the kernel debug log. */
#include "serial.h"
#include "io.h"

#define COM1 0x3F8

void serial_init(void)
{
	outb(COM1 + 1, 0x00);          /* disable interrupts        */
	outb(COM1 + 3, 0x80);          /* enable DLAB               */
	outb(COM1 + 0, 0x03);          /* divisor low  (38400 baud) */
	outb(COM1 + 1, 0x00);          /* divisor high              */
	outb(COM1 + 3, 0x03);          /* 8 bits, no parity, 1 stop */
	outb(COM1 + 2, 0xC7);          /* enable + clear FIFO       */
	outb(COM1 + 4, 0x0B);          /* RTS/DSR set               */
}

void serial_putc(char c)
{
	while ((inb(COM1 + 5) & 0x20) == 0)
		;
	outb(COM1, (unsigned char)c);
}

void serial_write(const char *s)
{
	for (; *s != '\0'; s++)
		serial_putc(*s);
}

void serial_write_dec(unsigned int value)
{
	char buf[11];
	int i = 0;

	if (value == 0) {
		serial_putc('0');
		return;
	}
	while (value > 0) {
		buf[i++] = (char)('0' + (value % 10));
		value /= 10;
	}
	while (i > 0)
		serial_putc(buf[--i]);
}

void serial_write_hex(unsigned int value)
{
	const char *digits = "0123456789abcdef";

	for (int shift = 28; shift >= 0; shift -= 4)
		serial_putc(digits[(value >> shift) & 0xF]);
}
