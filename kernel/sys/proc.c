/* Process control block + process table -- data layer only.
 *
 * This file owns the PCB array and a tiny allocator. It deliberately does NOT
 * touch the scheduler, the timer IRQ, or the context-switch machinery. Those
 * are layered on top in later commits. Until that wiring lands, the table just
 * tracks the single implicit "boot process" (PID 1) so the rest of the kernel
 * can already call proc_current() and get a stable answer.
 */
#include "defs.h"
#include "proc.h"

static struct proc table[PROC_MAX];
static struct proc *running;            /* NULL until proc_init() runs */

static void zero_proc(struct proc *p)
{
	p->pid        = 0;
	p->parent_pid = 0;
	p->state      = PROC_UNUSED;
	p->exit_code  = 0;
	p->kstack_top = 0;
	p->esp        = 0;
	p->cr3        = 0;
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
	boot->pid        = 1;
	boot->parent_pid = 0;
	boot->state      = PROC_RUNNING;
	copy_name(boot, "kinit");

	running = boot;
}

struct proc *proc_alloc(const char *name)
{
	for (int i = 0; i < PROC_MAX; i++) {
		if (table[i].state != PROC_UNUSED)
			continue;
		zero_proc(&table[i]);
		table[i].pid        = i + 1;       /* PID = slot index + 1 */
		table[i].parent_pid = running ? running->pid : 0;
		table[i].state      = PROC_EMBRYO;
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

	/* A single 4 KiB physical frame is enough for early kernel threads -- they
	 * never recurse deeply and own no large locals. If the table runs out of
	 * memory, fail closed and release the PCB slot so the caller can retry. */
	unsigned int frame = pmm_alloc();
	if (!frame) {
		proc_free(p);
		return 0;
	}

	p->kstack_top = frame + 4096;              /* stack grows down from the top */
	p->esp        = context_init(p->kstack_top, entry);
	p->cr3        = 0;                          /* share the kernel page dir */
	p->state      = PROC_READY;

	return p;
}
