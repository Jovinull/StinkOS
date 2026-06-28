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
	p->pid             = 0;
	p->parent_pid      = 0;
	p->state           = PROC_UNUSED;
	p->exit_code       = 0;
	p->kstack_top      = 0;
	p->esp             = 0;
	p->cr3             = 0;
	p->pending_signals = 0;
	p->priority        = 16;             /* default mid-priority */
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

/* Pick the next PROC_READY slot, preferring lower priority values (highest
 * priority wins). Within a priority level, round-robin from `from + 1` so
 * equal-prio peers share the CPU fairly. Returns NULL if nothing else is
 * runnable. */
static struct proc *next_ready(int from)
{
	int best_prio = 32;                       /* worse than any valid priority */
	int best_idx  = -1;

	for (int i = 1; i <= PROC_MAX; i++) {
		int idx = (from + i) % PROC_MAX;
		if (table[idx].state != PROC_READY)
			continue;
		if (table[idx].priority < best_prio) {
			best_prio = table[idx].priority;
			best_idx  = idx;
		}
	}
	return (best_idx >= 0) ? &table[best_idx] : 0;
}

void proc_yield(void)
{
	if (!running)
		return;                            /* proc_init() not run yet */

	int cur_idx = running->pid - 1;
	struct proc *next = next_ready(cur_idx);
	if (!next)
		return;                            /* no other ready thread */

	struct proc *prev = running;
	if (prev->state == PROC_RUNNING)
		prev->state = PROC_READY;          /* preempted, not blocked */
	next->state = PROC_RUNNING;
	running     = next;

	/* TODO §1 multitasking, step 2: swap CR3 to the incoming process's
	 * pgdir before swapping the stack. Order matters -- once
	 * context_switch returns into `next`, every memory access uses
	 * `next`'s address space. paging_switch is a no-op when next->cr3
	 * is 0 (the legacy shared-boot-pgdir mode), so the single-process
	 * boot path stays identical until step 3 starts handing out
	 * per-process pgdirs. */
	paging_switch((unsigned int *)next->cr3);
	context_switch(&prev->esp, next->esp);
}

int proc_reap(struct proc *p)
{
	if (!p || p->state != PROC_ZOMBIE)
		return -1;
	int code = p->exit_code;
	if (p->kstack_top) {
		/* kstack_top is the EXCLUSIVE top, so the frame base sits 4 KiB below. */
		pmm_free(p->kstack_top - 4096);
	}
	proc_free(p);
	return code;
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
		off = append_str(out, off, cap, state_label(p->state));
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
