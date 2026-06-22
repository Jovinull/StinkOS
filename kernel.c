/* StinkOS kernel - entry point in 32-bit protected mode.
 * The bootloader hands control here via call kernel_main after switching to PM.
 * VGA text output (0xB8000) for the user; serial (COM1) as a debug console. */

static inline void outb(unsigned short port, unsigned char val)
{
	__asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline unsigned char inb(unsigned short port)
{
	unsigned char ret;
	__asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
	return ret;
}

/* ---- VGA text console (0xB8000, 80x25) ---- */

#define VGA_TEXT  ((volatile unsigned short *)0xB8000)
#define VGA_COLS  80
#define VGA_ROWS  25
#define VGA_ATTR  0x07                 /* light grey on black */

static unsigned short cell(char c)
{
	return ((unsigned short)VGA_ATTR << 8) | (unsigned char)c;
}

static void vga_clear(void)
{
	for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
		VGA_TEXT[i] = cell(' ');
}

static void vga_put(const char *s, int row, int col)
{
	int i = row * VGA_COLS + col;

	for (; *s != '\0'; s++)
		VGA_TEXT[i++] = cell(*s);
}

/* ---- Serial debug console (COM1, 38400 8N1) ---- */

#define COM1 0x3F8

static void serial_init(void)
{
	outb(COM1 + 1, 0x00);          /* disable interrupts        */
	outb(COM1 + 3, 0x80);          /* enable DLAB               */
	outb(COM1 + 0, 0x03);          /* divisor low  (38400 baud) */
	outb(COM1 + 1, 0x00);          /* divisor high              */
	outb(COM1 + 3, 0x03);          /* 8 bits, no parity, 1 stop */
	outb(COM1 + 2, 0xC7);          /* enable + clear FIFO       */
	outb(COM1 + 4, 0x0B);          /* RTS/DSR set               */
}

static void serial_putc(char c)
{
	while ((inb(COM1 + 5) & 0x20) == 0)
		;
	outb(COM1, (unsigned char)c);
}

static void serial_write(const char *s)
{
	for (; *s != '\0'; s++)
		serial_putc(*s);
}

void kernel_main(void)
{
	serial_init();
	vga_clear();
	vga_put("StinkOS - 32-bit protected mode active.", 0, 0);
	serial_write("StinkOS: protected mode active\n");

	for (;;)
		__asm__ volatile ("hlt");
}
