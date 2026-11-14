/* Process control blocks, the scheduler and process lifetime.
 *
 * Scheduling model (see proc.h): the kernel never preempts itself. The PIT
 * switches processes only when it interrupts ring 3; kernel code gives up
 * the CPU explicitly through proc_yield / proc_sleep_ticks / proc_block.
 * Everything in this file therefore runs with at most one process inside
 * the kernel at a time; the only concurrent writer is proc_tick() /
 * proc_wake() from interrupt context, which only ever flips a SLEEPING
 * proc to READY.
 */
#include "defs.h"
#include "proc.h"
#include "memlayout.h"

static struct proc table[PROC_MAX];
static struct proc *running;            /* NULL until proc_init() runs */

/* Kernel stacks for slots 1..PROC_MAX-1 (slot 0 is the boot proc, which
 * owns the static stack in gdt.c). A static pool rather than PMM frames:
 * 8 KiB needs two contiguous frames, which the PMM cannot promise once
 * memory has churned. */
static unsigned char kstack_pool[PROC_MAX][KSTACK_SIZE] __attribute__((aligned(16)));

/* Two words at the lowest address of every kernel stack. The stack grows
 * down towards them; if they change, the stack overflowed into whatever
 * sits below and nothing the kernel knows can be trusted any more. */
#define KSTACK_CANARY0  0x4B415453u       /* "STAK" */
#define KSTACK_CANARY1  0x4B4E5453u       /* "STNK" */
#define KSTACK_FILL     0xCCu             /* untouched-stack pattern for the high-water mark */

static void kstack_arm(struct proc *p)
{
	unsigned int *base = (unsigned int *)p->kstack_base;
	base[0] = KSTACK_CANARY0;
	base[1] = KSTACK_CANARY1;
}

static void kstack_check(const struct proc *p)
{
	if (!p->kstack_base)
		return;
	const unsigned int *base = (const unsigned int *)p->kstack_base;
	if (base[0] == KSTACK_CANARY0 && base[1] == KSTACK_CANARY1)
		return;
	__asm__ volatile ("cli");
	serial_write("StinkOS: kernel stack overflow, pid ");
	serial_write_dec((unsigned int)p->pid);
	serial_write(" (");
	serial_write(p->name);
	serial_write(") - halted\n");
	for (;;)
		__asm__ volatile ("cli; hlt");
}

/* Bytes of the stack that have ever been written: everything above the
 * lowest byte that no longer holds the fill pattern. */
static unsigned int kstack_peak(const struct proc *p)
{
	const unsigned char *base = (const unsigned char *)p->kstack_base;
	unsigned int size = p->kstack_top - p->kstack_base;
	unsigned int i = 8;                        /* skip the canary */
	while (i < size && base[i] == KSTACK_FILL)
		i++;
	return size - i;
}

static void kstack_report(struct proc *p)
{
	unsigned int size = p->kstack_top - p->kstack_base;
	unsigned int peak = kstack_peak(p);
	if (peak <= size * 3u / 4u || peak <= p->kstack_warned)
		return;
	p->kstack_warned = peak;
	serial_write("proc: pid ");
	serial_write_dec((unsigned int)p->pid);
	serial_write(" used ");
	serial_write_dec(peak);
	serial_write(" of ");
	serial_write_dec(size);
	serial_write(" bytes of kernel stack\n");
}

void proc_kstack_audit(void)
{
	if (!running || !running->kstack_base)
		return;
	kstack_check(running);
	kstack_report(running);
}

static void zero_proc(struct proc *p)
{
	p->pid             = 0;
	p->parent_pid      = 0;
	p->state           = PROC_UNUSED;
	p->exit_code       = 0;
	p->kstack_top      = 0;
	p->kstack_base     = 0;
	p->kstack_warned   = 0;
	p->pipe_ends       = 0;
	p->last_run        = 0;
	p->esp             = 0;
	p->cr3             = 0;
	p->pending_signals = 0;
	p->priority        = 16;             /* default mid-priority */
	p->wake_tick       = 0;
	p->sleep_timed     = 0;
	p->suspended       = 0;
	p->killed          = 0;
	p->vm_heap_next    = 0;
	p->vm_fb_mapped    = 0;
	for (int i = 0; i < PROC_NSIG; i++)
		p->sig_handlers[i] = 0;
	vfs_fd_table_clear(p->fd_table);
	for (int i = 0; i < PROC_NAME_LEN; i++)
		p->name[i] = 0;
}

static void copy_name(struct proc *p, const char *name)
{
	if (!name) {
		p->name[0] = 0;
		return;
	}
	int i = 0;
	for (; i < PROC_NAME_LEN - 1 && name[i]; i++)
		p->name[i] = name[i];
	for (; i < PROC_NAME_LEN; i++)
		p->name[i] = 0;
}

void proc_init(void)
{
	for (int i = 0; i < PROC_MAX; i++)
		zero_proc(&table[i]);

	/* Slot 0 represents the boot process (PID 1). Everything the kernel was
	 * doing before this subsystem existed continues to run inside it. */
	struct proc *boot = &table[0];
	boot->pid         = 1;
	boot->parent_pid  = 0;
	boot->state       = PROC_RUNNING;
	boot->kstack_top  = gdt_boot_kstack_top();  /* §1: TSS.esp0 follows on switch */
	/* The boot proc owns the address space paging_init_user built. A zero
	 * cr3 means "kernel thread" to the scheduler, which would park it on
	 * the kernel-only tables the first time it is switched back in. */
	boot->cr3         = (unsigned int)paging_boot_pgdir();
	boot->kstack_base = gdt_boot_kstack_base();
	/* Nothing runs on this stack yet (boot is still on the early stack
	 * from kentry), so it can take the same fill pattern as the pool. */
	for (unsigned int k = 0; k < boot->kstack_top - boot->kstack_base; k++)
		((unsigned char *)boot->kstack_base)[k] = KSTACK_FILL;
	kstack_arm(boot);
	copy_name(boot, "kinit");

	running = boot;
}

struct proc *proc_alloc(const char *name)
{
	/* Slot 0 is the boot proc and is never recycled. */
	for (int i = 1; i < PROC_MAX; i++) {
		if (table[i].state != PROC_UNUSED)
			continue;
		zero_proc(&table[i]);
		table[i].pid         = i + 1;       /* PID = slot index + 1 */
		table[i].parent_pid  = running ? running->pid : 0;
		table[i].state       = PROC_EMBRYO;
		table[i].kstack_base = (unsigned int)kstack_pool[i];
		table[i].kstack_top  = table[i].kstack_base + KSTACK_SIZE;
		for (unsigned int k = 0; k < KSTACK_SIZE; k++)
			kstack_pool[i][k] = KSTACK_FILL;
		kstack_arm(&table[i]);
		copy_name(&table[i], name);
		return &table[i];
	}
	return 0;
}

void proc_free(struct proc *p)
{
	if (!p)
		return;
	zero_proc(p);
}

struct proc *proc_current(void)
{
	return running;
}

struct proc *proc_get(int pid)
{
	if (pid < 1 || pid > PROC_MAX)
		return 0;
	struct proc *p = &table[pid - 1];
	return (p->state == PROC_UNUSED) ? 0 : p;
}

int proc_count(void)
{
	int n = 0;
	for (int i = 0; i < PROC_MAX; i++)
		if (table[i].state != PROC_UNUSED)
			n++;
	return n;
}

/* Pre-build a kernel stack so the first context_switch into it pops sane
 * callee-saved regs and rets straight into `entry`.
 *
 * The asm switch (boot/context_asm.s) pops EDI, ESI, EBX, EBP, EFLAGS in that
 * order and then issues ret -- so we lay those six 4-byte words from the top
 * downward: [return-addr=entry][EFLAGS=0x202][EBP=0][EBX=0][ESI=0][EDI=0].
 * The ESP value handed to context_switch is the address of the EDI slot, i.e.
 * the lowest of the six words.
 *
 * EFLAGS bit 1 must always be 1 (reserved) and IF (bit 9) is set so the new
 * thread runs with interrupts on, matching the kernel's normal posture. */
unsigned int context_init(unsigned int kstack_top, void (*entry)(void))
{
	unsigned int *sp = (unsigned int *)kstack_top;

	*--sp = (unsigned int)entry;               /* return address: ret jumps here */
	*--sp = 0x00000202;                         /* EFLAGS: reserved bit + IF      */
	*--sp = 0;                                  /* EBP                            */
	*--sp = 0;                                  /* EBX                            */
	*--sp = 0;                                  /* ESI                            */
	*--sp = 0;                                  /* EDI                            */

	return (unsigned int)sp;
}

struct proc *proc_kthread_create(const char *name, void (*entry)(void))
{
	if (!entry)
		return 0;

	struct proc *p = proc_alloc(name);
	if (!p)
		return 0;

	p->esp        = context_init(p->kstack_top, entry);
	p->cr3        = 0;                          /* runs on whatever address space is loaded */
	p->parent_pid = 0;                          /* nobody waits for a kernel thread */
	p->state      = PROC_READY;

	return p;
}

/* Pick the next runnable slot: the highest priority (lowest value) wins,
 * and among equals the one that has waited longest since its last turn.
 *
 * Least-recently-run rather than "the next slot after the current one":
 * a high-priority kernel thread that wakes every tick (kwm, knet) becomes
 * the current slot each time, and a scan that starts from it keeps handing
 * the CPU to the same lower-priority process -- whichever sits right after
 * the thread in the table -- while the others starve. Returns NULL if
 * nothing else is runnable. */
static unsigned int sched_seq;

static struct proc *next_ready(int from)
{
	int best_prio = 32;                       /* worse than any valid priority */
	int best_idx  = -1;

	for (int i = 1; i <= PROC_MAX; i++) {
		int idx = (from + i) % PROC_MAX;
		if (table[idx].state != PROC_READY || table[idx].suspended)
			continue;
		if (table[idx].priority < best_prio ||
		    (table[idx].priority == best_prio &&
		     (int)(table[idx].last_run - table[best_idx].last_run) < 0)) {
			best_prio = table[idx].priority;
			best_idx  = idx;
		}
	}
	return (best_idx >= 0) ? &table[best_idx] : 0;
}

static unsigned int irq_save(void)
{
	unsigned int flags;
	__asm__ volatile ("pushf; pop %0; cli" : "=r"(flags));
	return flags;
}

static void irq_restore(unsigned int flags)
{
	if (flags & 0x200u)
		__asm__ volatile ("sti");
}

static volatile int need_resched;       /* a tick landed in kernel mode */

void proc_request_resched(void)
{
	need_resched = 1;
}

void proc_resched_point(void)
{
	if (need_resched)
		proc_yield();
}

int proc_yield(void)
{
	if (!running)
		return 0;                          /* proc_init() not run yet */
	need_resched = 0;

	/* Interrupts stay off across the switch itself: the PCB states, CR3,
	 * TSS.esp0 and the saved ESP must change as one step. Each proc gets
	 * its own flags back when it is switched in again. */
	unsigned int flags = irq_save();

	int cur_idx = running->pid - 1;
	struct proc *next = next_ready(cur_idx);
	if (!next || next == running) {
		irq_restore(flags);
		return 0;                          /* nobody else can run */
	}

	struct proc *prev = running;
	kstack_check(prev);
	if (prev->state == PROC_RUNNING)
		prev->state = PROC_READY;          /* preempted, not blocked */
	next->state    = PROC_RUNNING;
	next->last_run = ++sched_seq;
	running        = next;

	paging_vm_save(&prev->vm_heap_next, &prev->vm_fb_mapped);
	paging_switch((unsigned int *)next->cr3);
	paging_vm_load(next->vm_heap_next, next->vm_fb_mapped);
	/* §1 step 5.5 ROOT-CAUSE FIX: TSS.esp0 selects which kernel stack
	 * the CPU loads on the NEXT ring-3 -> ring-0 trap. Before this fix
	 * it pointed at the static kernel_stack[] BSS regardless of which
	 * proc is running, so two procs trapping into the kernel via int
	 * 0x80 / IRQ would share the same stack -- the second one would
	 * overwrite the first one's saved frame and the first one's
	 * context_switch resume would `ret` into garbage (which happened
	 * to be the address of trap_return, hence the long-running
	 * misleading #GP at trap_return's mov %ax,%ds). */
	if (next->kstack_top)
		tss_set_kernel_stack(next->kstack_top);
	context_switch(&prev->esp, next->esp);

	/* Back on `prev`'s stack, some time later. Whoever exited meanwhile
	 * and has nobody to wait for it is safe to free now: it is not us. */
	proc_reap_orphans();
	irq_restore(flags);
	return 1;
}

void proc_wake(struct proc *p)
{
	if (!p)
		return;
	unsigned int flags = irq_save();
	if (p->state == PROC_SLEEPING) {
		p->sleep_timed = 0;
		p->state = PROC_READY;
	}
	irq_restore(flags);
}

void proc_tick(unsigned int now)
{
	for (int i = 0; i < PROC_MAX; i++) {
		struct proc *p = &table[i];
		if (p->state != PROC_SLEEPING || !p->sleep_timed)
			continue;
		if ((int)(now - p->wake_tick) < 0)
			continue;                      /* not due yet (wrap-safe) */
		p->sleep_timed = 0;
		p->state = PROC_READY;
	}
}

/* Core of every blocking call. The caller has marked `running` SLEEPING
 * (with interrupts off, so a wake-up cannot slip in between its check and
 * the state change). Run other procs until someone makes us READY again;
 * with nobody runnable, idle in hlt and let an interrupt do it. */
static void block_current(unsigned int flags)
{
	struct proc *me = running;
	for (;;) {
		proc_yield();
		if (me->state != PROC_SLEEPING)
			break;
		__asm__ volatile ("sti; hlt; cli");
		if (me->state != PROC_SLEEPING)
			break;
	}
	me->state = PROC_RUNNING;
	irq_restore(flags);
}

void proc_sleep_ticks(unsigned int ticks)
{
	if (!running || ticks == 0)
		return;
	unsigned int flags = irq_save();
	running->wake_tick   = pit_ticks() + ticks;
	running->sleep_timed = 1;
	running->state       = PROC_SLEEPING;
	block_current(flags);
}

void proc_block(unsigned int max_ticks)
{
	if (!running)
		return;
	unsigned int flags = irq_save();
	running->sleep_timed = max_ticks ? 1 : 0;
	running->wake_tick   = pit_ticks() + max_ticks;
	running->state       = PROC_SLEEPING;
	block_current(flags);
}

void proc_idle(void)
{
	if (!proc_yield())
		__asm__ volatile ("hlt");
}

/* ---- lifetime ------------------------------------------------------------ */

void proc_reset_image_state(struct proc *p)
{
	if (!p)
		return;
	p->pending_signals = 0;
	for (int i = 0; i < PROC_NSIG; i++)
		p->sig_handlers[i] = 0;
	if (p->pid == 1) {                         /* the foreground program */
		keyboard_flush();
		mouse_reset_delta();
	}
}

static void reap_slot(struct proc *p)
{
	kstack_check(p);
	if (p->kstack_base)
		kstack_report(p);
	if (p->cr3)
		paging_destroy_user_pgdir((unsigned int *)p->cr3);
	proc_free(p);
}

int proc_reap(struct proc *p)
{
	if (!p || p->state != PROC_ZOMBIE || p == running)
		return -1;
	int code = p->exit_code;
	reap_slot(p);
	return code;
}

void proc_reap_orphans(void)
{
	for (int i = 1; i < PROC_MAX; i++) {
		struct proc *p = &table[i];
		if (p->state == PROC_ZOMBIE && p->parent_pid == 0 && p != running)
			reap_slot(p);
	}
}

void proc_abandon_children(struct proc *parent)
{
	if (!parent)
		return;
	for (int i = 1; i < PROC_MAX; i++) {
		struct proc *c = &table[i];
		if (c->state == PROC_UNUSED || c == parent || c->parent_pid != parent->pid)
			continue;
		if (c->state == PROC_ZOMBIE && c != running)
			reap_slot(c);
		else
			c->parent_pid = 0;             /* the orphan reaper takes it when it exits */
	}
}

int proc_kill(struct proc *p)
{
	if (!p || p->pid <= 1 || p->state == PROC_ZOMBIE ||
	    p->state == PROC_EMBRYO || p->state == PROC_UNUSED)
		return -1;
	if (!p->killed) {
		p->killed    = 1;
		p->exit_code = 137;                /* SIGKILL exit-code convention */
	}
	p->suspended = 0;
	proc_wake(p);
	return 0;
}

void proc_exit_current(void)
{
	struct proc *me = running;
	__asm__ volatile ("cli");

	proc_abandon_children(me);

	struct proc *parent = proc_get(me->parent_pid);
	if (parent && parent->state != PROC_ZOMBIE)
		proc_wake(parent);                 /* it may be blocked in wait() */
	else
		me->parent_pid = 0;                /* nobody left to collect us */

	me->state = PROC_ZOMBIE;
	/* We keep running on our own kernel stack until someone else can take
	 * the CPU; whoever reaps us does so from a different stack. */
	for (;;) {
		proc_yield();
		__asm__ volatile ("sti; hlt; cli");
	}
}

struct proc *proc_find_zombie_child(int pid)
{
	if (!running)
		return 0;
	for (int i = 0; i < PROC_MAX; i++) {
		struct proc *p = &table[i];
		if (p->state != PROC_ZOMBIE)
			continue;
		if (p->parent_pid != running->pid)
			continue;
		if (pid > 0 && p->pid != pid)
			continue;
		return p;
	}
	return 0;
}

int proc_has_living_child(void)
{
	if (!running)
		return 0;
	for (int i = 0; i < PROC_MAX; i++) {
		struct proc *p = &table[i];
		if (p->state == PROC_UNUSED || p->state == PROC_ZOMBIE)
			continue;
		if (p->parent_pid == running->pid)
			return 1;
	}
	return 0;
}

static const char *state_label(enum proc_state s)
{
	switch (s) {
	case PROC_UNUSED:   return "FREE";
	case PROC_EMBRYO:   return "EMBR";
	case PROC_READY:    return "READ";
	case PROC_RUNNING:  return "RUN ";
	case PROC_SLEEPING: return "SLEP";
	case PROC_ZOMBIE:   return "ZOMB";
	}
	return "????";
}

static unsigned int append_str(char *out, unsigned int off, unsigned int cap,
                               const char *s)
{
	while (*s && off + 1 < cap)
		out[off++] = *s++;
	return off;
}

static unsigned int append_dec(char *out, unsigned int off, unsigned int cap,
                               unsigned int v, int width)
{
	char tmp[12];
	int n = 0;
	if (v == 0) tmp[n++] = '0';
	while (v > 0 && n < 11) { tmp[n++] = '0' + (v % 10); v /= 10; }
	while (n < width && off + 1 < cap) out[off++] = ' ';
	for (int i = n - 1; i >= 0 && off + 1 < cap; i--) out[off++] = tmp[i];
	return off;
}

unsigned int proc_snapshot(char *out, unsigned int cap)
{
	if (!out || cap == 0)
		return 0;
	unsigned int off = 0;
	off = append_str(out, off, cap, "PID  STATE PRIO PARENT NAME\n");
	for (int i = 0; i < PROC_MAX; i++) {
		struct proc *p = &table[i];
		if (p->state == PROC_UNUSED)
			continue;
		off = append_dec(out, off, cap, (unsigned int)p->pid, 3);
		if (off + 1 < cap) out[off++] = ' ';
		off = append_str(out, off, cap, " ");
		off = append_str(out, off, cap, p->suspended ? "SLEP" : state_label(p->state));
		if (off + 1 < cap) out[off++] = ' ';
		off = append_dec(out, off, cap, (unsigned int)p->priority, 3);
		if (off + 1 < cap) out[off++] = ' ';
		off = append_dec(out, off, cap, (unsigned int)p->parent_pid, 5);
		if (off + 1 < cap) out[off++] = ' ';
		for (int k = 0; k < PROC_NAME_LEN && p->name[k] && off + 1 < cap; k++)
			out[off++] = p->name[k];
		if (off + 1 < cap) out[off++] = '\n';
	}
	if (off < cap)
		out[off] = '\0';
	return off;
}
