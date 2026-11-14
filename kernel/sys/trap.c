/* Low-level interrupt machinery: IDT setup, PIC remap, the PIT timer and the
 * C-side trap/IRQ handlers the assembly stubs call into. The syscall layer
 * (int 0x80) lives in syscall.c; this file just routes there. */
#include "defs.h"
#include "paging.h"
#include "memlayout.h"
#include "win.h"

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

unsigned int pit_ticks(void) { return ticks; }

/* A killed process never gets back to ring 3: called on the way out of
 * every trap that came from user mode. The process tears itself down in
 * its own context (its address space, its kernel stack), which is the only
 * place that is safe to do from. */
static void die_if_killed(void)
{
	struct proc *cur = proc_current();
	if (cur && cur->killed && cur->pid > 1) {
		__asm__ volatile ("sti");
		app_return();                      /* does not return */
	}
}

void isr_handler(struct regs *r)
{
	if (r->int_no == 128) {                    /* int 0x80 syscall */
		syscall_dispatch(r);
		proc_resched_point();              /* a tick came in while we were in here */
		die_if_killed();                   /* ... or a SYS_KILL */
		return;
	}

	if ((r->cs & 3) == 3) {                    /* fault from ring 3 */
		unsigned int cr2 = 0;
		__asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
		/* COW resolution: #PF (vector 14) with err_code bit 0 set
		 * (protection violation, not page-missing) + bit 1 set (write)
		 * + bit 2 set (user) is the signature of a write to a shared
		 * post-fork page. paging_handle_cow_fault returns 1 when the
		 * page has PG_COW and was privatized / unlocked; we simply
		 * iret back and the instruction retries. Returns 0 for real
		 * W^X violations or non-COW protection faults -- those fall
		 * through to the kill path below. */
		if (r->int_no == 14 && (r->err_code & 0x7u) == 0x7u &&
		    paging_handle_cow_fault(cr2))
			return;
		serial_write("app: fault, killed (exception ");
		serial_write_dec(r->int_no);
		serial_write(", eip=0x");
		serial_write_hex(r->eip);
		serial_write(", cr2=0x");
		serial_write_hex(cr2);
		serial_write(")\n");
		struct proc *dead = proc_current();
		if (dead)
			dead->exit_code = 128 + 11;    /* what a parent's wait() sees: "killed by a fault" */
		app_return();                      /* back to the shell or menu (no return) */
	}

	unsigned int cr2 = 0;
	__asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

	/* Ring-0 write to a present user page that is read-only because it is
	 * still COW-shared (CR0.WP makes the kernel fault on those too).
	 * Syscalls privatize their output buffers up front through
	 * paging_user_range_writable, so reaching this is a missed call site:
	 * resolve it the same way a user fault would be, and say so. */
	if (r->int_no == 14 && (r->err_code & 0x7u) == 0x3u &&
	    cr2 < KERNBASE && paging_handle_cow_fault(cr2)) {
		serial_write("paging: kernel-mode COW fault at eip=0x");
		serial_write_hex(r->eip);
		serial_write(" (unvalidated user write)\n");
		return;
	}

	serial_write("StinkOS: kernel exception ");
	serial_write_dec(r->int_no);
	serial_write(" at eip=0x");
	serial_write_hex(r->eip);
	serial_write(" cr2=0x");
	serial_write_hex(cr2);
	serial_write(" - halted\n");

	for (;;)
		__asm__ volatile ("cli; hlt");
}

void irq_handler(struct regs *r)
{
	int from_user = (r->cs & 3) == 3;
	int preempt = 0;
	if (r->int_no == 32) {                     /* IRQ0: PIT timer */
		ticks++;
		if (ticks <= 3)
			serial_write("StinkOS: timer tick\n");
		timer_tick();                       /* fire any expired one-shots */
		win_tick();                         /* compositor: ~30fps */
		proc_tick(ticks);                   /* wake sleepers that are due */
		if (ticks % 50 == 0)
			proc_kstack_audit();        /* canary + high-water mark */
		if (from_user)
			audio_reclaim();            /* no kernel code interrupted: PMM is safe */
		/* The kernel does not preempt itself: only a tick that
		 * interrupted ring 3 may switch processes. Kernel code is
		 * rescheduled where it explicitly yields or blocks. */
		preempt = from_user;
		if (!from_user)
			proc_request_resched();     /* switch on the way back to ring 3 */
	} else if (r->int_no == 33) {              /* IRQ1: keyboard */
		keyboard_handle();
	} else if (r->int_no == 37) {              /* IRQ5: Sound Blaster 16 */
		audio_handle_irq();
	} else if (r->int_no == 40) {              /* IRQ8: CMOS RTC alarm */
		rtc_handle_irq();
	} else if (r->int_no == 44) {              /* IRQ12: PS/2 mouse */
		mouse_handle(inb(0x60));
	}

	/* PCI INTx lines are shared and the BIOS-assigned vector varies, so
	 * call the e1000 IRQ entry on every hardware IRQ. The handler reads
	 * ICR (clears on read) and no-ops when no descriptor is done. */
	if (r->int_no >= 32 && r->int_no <= 47 && e1000_handle_irq()) {
		net_irq_wake();                     /* frames are parsed by the knet thread */
		if (from_user)
			preempt = 1;                /* ... as soon as this interrupt returns */
		else
			proc_request_resched();
	}

	if (r->int_no >= 40)                        /* from the slave PIC */
		outb(0xA0, 0x20);
	outb(0x20, 0x20);                           /* end of interrupt */

	/* EOI must precede the yield: context_switch may not return for many ticks,
	 * and the PIC won't deliver further IRQs until the current one is acked. */
	if (preempt)
		proc_yield();
	if (from_user)
		die_if_killed();
}
