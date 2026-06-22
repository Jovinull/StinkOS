/* StinkOS kernel - entry point in 32-bit protected mode.
 * The bootloader sets a VBE linear-framebuffer mode and hands control here. */
#include "io.h"
#include "serial.h"
#include "interrupts.h"
#include "keyboard.h"
#include "vbe.h"
#include "fb.h"
#include "pmm.h"
#include "paging.h"

void kernel_main(void)
{
	serial_init();
	serial_write("StinkOS: protected mode active\n");

	pmm_init(0x100000, 0x2000000);          /* manage 1 MiB .. 32 MiB */
	paging_init();
	serial_write("paging: enabled\n");

	unsigned int frame = pmm_alloc();
	serial_write("pmm: frame 0x");
	serial_write_hex(frame);
	serial_putc('\n');
	pmm_free(frame);

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

		fb_init(&vm);
		fb_fill(0x001022);                                  /* dark background */
		fb_rect(112, 84, vm.width - 224, vm.height - 168, 0x3050C0);
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
