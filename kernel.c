/* StinkOS kernel - entry point in 32-bit protected mode.
 * The bootloader hands control here via call kernel_main after switching to PM. */
#include "io.h"
#include "serial.h"
#include "interrupts.h"
#include "keyboard.h"
#include "vbe.h"

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

void kernel_main(void)
{
	serial_init();
	vga_clear();
	vga_put("StinkOS - 32-bit protected mode active.", 0, 0);
	serial_write("StinkOS: protected mode active\n");

	struct vbe_mode vm;
	vbe_read(&vm);
	if (vm.valid) {
		serial_write("vbe: ");
		serial_write_dec(vm.width);
		serial_putc('x');
		serial_write_dec(vm.height);
		serial_putc('x');
		serial_write_dec(vm.bpp);
		serial_write(" lfb 0x");
		serial_write_hex(vm.framebuffer);
		serial_putc('\n');
	} else {
		serial_write("vbe: unavailable\n");
	}

	interrupts_init();
	pit_init(100);
	keyboard_init();
	__asm__ volatile ("sti");
	serial_write("StinkOS: interrupts enabled\n");

	for (;;)
		__asm__ volatile ("hlt");
}
