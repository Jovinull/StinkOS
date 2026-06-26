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
#include "gdt.h"
#include "menu.h"
#include "mouse.h"
#include "audio.h"

void kernel_main(void)
{
	serial_init();
	serial_write("StinkOS: protected mode active\n");

	gdt_init();
	serial_write("gdt: kernel+user segments and tss loaded\n");

	pmm_init(0x100000, 0x2000000);          /* manage 1 MiB .. 32 MiB */
	paging_init();
	paging_init_user();                     /* isolated 4 KiB userland region */
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
	} else {
		serial_write("vbe: unavailable\n");
	}

	interrupts_init();
	pit_init(100);
	keyboard_init();
	if (vm.valid)
		mouse_init(vm.width, vm.height);   /* before sti: the PS/2 handshake polls
		                                      the 8042, so IRQ12 must not race it */
	audio_init();                              /* probes SB16; no-op if -device sb16 absent */
	audio_start_output();                      /* arm the DMA loop; silent until mixer fills */
	__asm__ volatile ("sti");
	serial_write("StinkOS: interrupts enabled\n");

	if (vm.valid)
		menu_run();                     /* show the menu and launch an app */

	for (;;)
		__asm__ volatile ("hlt");
}
