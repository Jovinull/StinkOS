/* StinkOS kernel - entry point in 32-bit protected mode.
 * The bootloader sets a VBE linear-framebuffer mode and hands control here. */
#include "defs.h"
#include "memlayout.h"

/* Linker-script symbol pointing at the END of the kernel image. With the
 * higher-half link, __bss_end is a VIRT address (~0x801XXXXX); the PMM
 * tracks PHYS, so we V2P() before handing the watermark off. PMM still
 * starts allocating frames AFTER the kernel image so no allocator stomps
 * on .text/.bss. */
extern char __bss_end[];

void kmain(void)
{
	serial_init();
	serial_write("StinkOS: protected mode active\n");
	bootdiag_add("serial: com1", BOOT_OK);

	gdt_init();
	serial_write("gdt: kernel+user segments and tss loaded\n");
	bootdiag_add("cpu: gdt+tss", BOOT_OK);

	pmm_init(V2P((unsigned int)__bss_end), 0x2000000);   /* kernel-end .. 32 MiB */
	paging_init();
	paging_init_user();                     /* isolated 4 KiB userland region */
	serial_write("paging: enabled\n");
	bootdiag_add("mem: paging", BOOT_OK);

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
	bootdiag_add("video: vbe", vm.valid ? BOOT_OK : BOOT_FAIL);

	interrupts_init();
	bootdiag_add("cpu: idt+pic", BOOT_OK);
	pit_init(100);
	bootdiag_add("timer: pit", BOOT_OK);
	proc_init();                               /* reserves PID 1 for the kernel boot path */
	bootdiag_add("proc: pid 1", BOOT_OK);
	timer_init();                              /* one-shot timer table */
	bootdiag_add("timer: subsystem", BOOT_OK);
	keyboard_init();
	bootdiag_add("input: kbd", BOOT_OK);
	if (vm.valid)
		mouse_init(vm.width, vm.height);   /* before sti: the PS/2 handshake polls
		                                      the 8042, so IRQ12 must not race it */
	bootdiag_add("input: mouse", vm.valid ? BOOT_OK : BOOT_ABSENT);
	audio_init();                              /* probes SB16; no-op if -device sb16 absent */
	audio_start_output();                      /* arm the DMA loop; silent until mixer fills */
	bootdiag_add("audio: sb16", audio_present() ? BOOT_OK : BOOT_ABSENT);
	pci_scan();                                /* log every PCI device for visibility */
	bootdiag_add("bus: pci", BOOT_OK);
	ata_dma_init();                            /* enable PIIX Bus Master DMA if present */
	{
		char model[41];
		unsigned int sectors;
		bootdiag_add("disk: ata",
		             ata_identify(model, &sectors) == 0 ? BOOT_OK : BOOT_ABSENT);
	}
	e1000_init();                              /* probes Intel 82540EM; no-op if absent */
	bootdiag_add("net: e1000", e1000_present() ? BOOT_OK : BOOT_ABSENT);
	net_init();                                /* caches local MAC for L2 emit */
	dhcp_start();                              /* DISCOVER goes out; reply lands via UDP cb */
	bootdiag_add("net: dhcp", e1000_present() ? BOOT_OK : BOOT_ABSENT);
	__asm__ volatile ("sti");
	serial_write("StinkOS: interrupts enabled\n");

	if (vm.valid) {
		bootdiag_show();                /* POST panel, then hand off to the menu */
		menu_run();                     /* show the menu and launch an app */
	}

	for (;;)
		__asm__ volatile ("hlt");
}
