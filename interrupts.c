/* IDT setup, PIC remap, PIT timer and the C-side interrupt handlers. */
#include "interrupts.h"
#include "serial.h"
#include "keyboard.h"
#include "fb.h"
#include "pmm.h"
#include "paging.h"
#include "menu.h"
#include "io.h"

/* ---- assembly stubs ---- */
extern void idt_load(unsigned int idt_ptr_addr);
extern void isr0(void),  isr1(void),  isr2(void),  isr3(void),  isr4(void);
extern void isr5(void),  isr6(void),  isr7(void),  isr8(void),  isr9(void);
extern void isr10(void), isr11(void), isr12(void), isr13(void), isr14(void);
extern void isr15(void), isr16(void), isr17(void), isr18(void), isr19(void);
extern void isr20(void), isr21(void), isr22(void), isr23(void), isr24(void);
extern void isr25(void), isr26(void), isr27(void), isr28(void), isr29(void);
extern void isr30(void), isr31(void);
extern void irq0(void),  irq1(void),  irq2(void),  irq3(void),  irq4(void);
extern void irq5(void),  irq6(void),  irq7(void),  irq8(void),  irq9(void);
extern void irq10(void), irq11(void), irq12(void), irq13(void), irq14(void);
extern void irq15(void);
extern void isr128(void);                 /* int 0x80 syscall entry */

/* ---- IDT ---- */
struct idt_entry {
	unsigned short base_lo;
	unsigned short sel;
	unsigned char  always0;
	unsigned char  flags;
	unsigned short base_hi;
} __attribute__((packed));

struct idt_ptr {
	unsigned short limit;
	unsigned int   base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idtp;

static void set_gate(int n, unsigned int base, unsigned short sel, unsigned char flags)
{
	idt[n].base_lo = base & 0xFFFF;
	idt[n].base_hi = (base >> 16) & 0xFFFF;
	idt[n].sel     = sel;
	idt[n].always0 = 0;
	idt[n].flags   = flags;
}

/* Remap the 8259 PICs so IRQ0..15 land on vectors 32..47 (clear of the CPU
 * exception range 0..31). */
static void pic_remap(void)
{
	outb(0x20, 0x11); io_wait();   /* start init, master */
	outb(0xA0, 0x11); io_wait();   /* start init, slave  */
	outb(0x21, 0x20); io_wait();   /* master vector base 32 */
	outb(0xA1, 0x28); io_wait();   /* slave  vector base 40 */
	outb(0x21, 0x04); io_wait();   /* tell master: slave on IRQ2 */
	outb(0xA1, 0x02); io_wait();   /* tell slave its cascade identity */
	outb(0x21, 0x01); io_wait();   /* 8086 mode */
	outb(0xA1, 0x01); io_wait();
	outb(0x21, 0x00); io_wait();   /* unmask all IRQs */
	outb(0xA1, 0x00); io_wait();
}

void interrupts_init(void)
{
	idtp.limit = sizeof(idt) - 1;
	idtp.base  = (unsigned int)&idt;

	for (int i = 0; i < 256; i++)
		set_gate(i, 0, 0, 0);

	pic_remap();

	set_gate(0,  (unsigned int)isr0,  0x08, 0x8E);
	set_gate(1,  (unsigned int)isr1,  0x08, 0x8E);
	set_gate(2,  (unsigned int)isr2,  0x08, 0x8E);
	set_gate(3,  (unsigned int)isr3,  0x08, 0x8E);
	set_gate(4,  (unsigned int)isr4,  0x08, 0x8E);
	set_gate(5,  (unsigned int)isr5,  0x08, 0x8E);
	set_gate(6,  (unsigned int)isr6,  0x08, 0x8E);
	set_gate(7,  (unsigned int)isr7,  0x08, 0x8E);
	set_gate(8,  (unsigned int)isr8,  0x08, 0x8E);
	set_gate(9,  (unsigned int)isr9,  0x08, 0x8E);
	set_gate(10, (unsigned int)isr10, 0x08, 0x8E);
	set_gate(11, (unsigned int)isr11, 0x08, 0x8E);
	set_gate(12, (unsigned int)isr12, 0x08, 0x8E);
	set_gate(13, (unsigned int)isr13, 0x08, 0x8E);
	set_gate(14, (unsigned int)isr14, 0x08, 0x8E);
	set_gate(15, (unsigned int)isr15, 0x08, 0x8E);
	set_gate(16, (unsigned int)isr16, 0x08, 0x8E);
	set_gate(17, (unsigned int)isr17, 0x08, 0x8E);
	set_gate(18, (unsigned int)isr18, 0x08, 0x8E);
	set_gate(19, (unsigned int)isr19, 0x08, 0x8E);
	set_gate(20, (unsigned int)isr20, 0x08, 0x8E);
	set_gate(21, (unsigned int)isr21, 0x08, 0x8E);
	set_gate(22, (unsigned int)isr22, 0x08, 0x8E);
	set_gate(23, (unsigned int)isr23, 0x08, 0x8E);
	set_gate(24, (unsigned int)isr24, 0x08, 0x8E);
	set_gate(25, (unsigned int)isr25, 0x08, 0x8E);
	set_gate(26, (unsigned int)isr26, 0x08, 0x8E);
	set_gate(27, (unsigned int)isr27, 0x08, 0x8E);
	set_gate(28, (unsigned int)isr28, 0x08, 0x8E);
	set_gate(29, (unsigned int)isr29, 0x08, 0x8E);
	set_gate(30, (unsigned int)isr30, 0x08, 0x8E);
	set_gate(31, (unsigned int)isr31, 0x08, 0x8E);

	set_gate(32, (unsigned int)irq0,  0x08, 0x8E);
	set_gate(33, (unsigned int)irq1,  0x08, 0x8E);
	set_gate(34, (unsigned int)irq2,  0x08, 0x8E);
	set_gate(35, (unsigned int)irq3,  0x08, 0x8E);
	set_gate(36, (unsigned int)irq4,  0x08, 0x8E);
	set_gate(37, (unsigned int)irq5,  0x08, 0x8E);
	set_gate(38, (unsigned int)irq6,  0x08, 0x8E);
	set_gate(39, (unsigned int)irq7,  0x08, 0x8E);
	set_gate(40, (unsigned int)irq8,  0x08, 0x8E);
	set_gate(41, (unsigned int)irq9,  0x08, 0x8E);
	set_gate(42, (unsigned int)irq10, 0x08, 0x8E);
	set_gate(43, (unsigned int)irq11, 0x08, 0x8E);
	set_gate(44, (unsigned int)irq12, 0x08, 0x8E);
	set_gate(45, (unsigned int)irq13, 0x08, 0x8E);
	set_gate(46, (unsigned int)irq14, 0x08, 0x8E);
	set_gate(47, (unsigned int)irq15, 0x08, 0x8E);

	/* syscall gate: DPL 3 so ring-3 code may invoke int 0x80 */
	set_gate(0x80, (unsigned int)isr128, 0x08, 0xEE);

	idt_load((unsigned int)&idtp);
}

void pit_init(unsigned int hz)
{
	unsigned int divisor = 1193180 / hz;

	outb(0x43, 0x36);                          /* channel 0, rate generator */
	outb(0x40, (unsigned char)(divisor & 0xFF));
	outb(0x40, (unsigned char)((divisor >> 8) & 0xFF));
}

/* ---- C handlers (called from the stubs) ---- */

static volatile unsigned int ticks = 0;        /* PIT ticks since boot */

/* System calls: eax = number, ebx = arg. Result returned in eax. */
static void syscall_dispatch(struct regs *r)
{
	switch (r->eax) {
	case 1:                                    /* SYS_LOG: ebx = string */
		serial_write("ring3: ");
		serial_write((const char *)r->ebx);
		serial_putc('\n');
		r->eax = 0;
		break;
	case 2:                                    /* SYS_DRAW: ebx=x ecx=y edx=rgb */
		fb_putpixel(r->ebx, r->ecx, r->edx);
		r->eax = 0;
		break;
	case 3:                                    /* SYS_GETKEY: -> char or 0 */
		r->eax = (unsigned char)keyboard_getchar();
		break;
	case 4:                                    /* SYS_ALLOC: -> user page or 0 */
		r->eax = paging_user_alloc();
		break;
	case 5:                                    /* SYS_EXIT: return to the menu */
		menu_exit();                       /* does not return */
		break;
	case 6:                                    /* SYS_TICKS: -> PIT ticks */
		r->eax = ticks;
		break;
	default:
		r->eax = (unsigned int)-1;
		break;
	}
}

void isr_handler(struct regs *r)
{
	if (r->int_no == 128) {                    /* int 0x80 syscall */
		syscall_dispatch(r);
		return;
	}

	if ((r->cs & 3) == 3) {                    /* fault from ring 3: kill the app */
		serial_write("app: fault, killed (exception ");
		serial_write_dec(r->int_no);
		serial_write(")\n");
		menu_exit();                       /* return to the menu (no return) */
	}

	serial_write("StinkOS: kernel exception ");
	serial_write_dec(r->int_no);
	serial_write(" - halted\n");

	for (;;)
		__asm__ volatile ("cli; hlt");
}

void irq_handler(struct regs *r)
{
	if (r->int_no == 32) {                     /* IRQ0: PIT timer */
		ticks++;
		if (ticks <= 3)
			serial_write("StinkOS: timer tick\n");
	} else if (r->int_no == 33) {              /* IRQ1: keyboard */
		keyboard_handle();
	}

	if (r->int_no >= 40)                        /* from the slave PIC */
		outb(0xA0, 0x20);
	outb(0x20, 0x20);                           /* end of interrupt */
}
