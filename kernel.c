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
#include "ata.h"

extern void enter_user_mode(unsigned int entry, unsigned int user_stack);

/* App loaded from a raw disk slot and run in ring 3. */
#define APP_LBA      64                 /* sector where the app binary lives */
#define APP_SECTORS  4
#define APP_ADDR     0x400000           /* load/run address (app is linked here) */

static unsigned char user_stack[4096] __attribute__((aligned(16)));

void kernel_main(void)
{
	serial_init();
	serial_write("StinkOS: protected mode active\n");

	gdt_init();
	serial_write("gdt: kernel+user segments and tss loaded\n");

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
		fb_text(120, 90, "STINKOS", 0xFFFFFF);
	} else {
		serial_write("vbe: unavailable\n");
	}

	interrupts_init();
	pit_init(100);
	keyboard_init();
	__asm__ volatile ("sti");
	serial_write("StinkOS: interrupts enabled\n");

	/* Load the app from its raw disk slot and run it in ring 3. */
	ata_read(APP_LBA, APP_SECTORS, (void *)APP_ADDR);
	serial_write("loader: app loaded from disk slot\n");
	paging_set_user(APP_ADDR);                       /* app code/data region */
	paging_set_user((unsigned int)user_stack);       /* user stack region */
	enter_user_mode(APP_ADDR,
	                (unsigned int)(user_stack + sizeof(user_stack)));

	for (;;)
		__asm__ volatile ("hlt");
}
