/* Process control block + process table.
 *
 * First brick of the multitasking subsystem. This commit only introduces the
 * data and the allocator -- no scheduler, no context switch, no IRQ tick hook.
 * Those land in follow-up commits so each step stays trivially reviewable and
 * keeps the kernel boot path working unchanged.
 */
#ifndef PROC_H
#define PROC_H

#include "interrupts.h"   /* struct regs */

#define PROC_MAX        16          /* hard cap on concurrent processes */
#define PROC_NAME_LEN   16          /* incl. NUL */

enum proc_state {
	PROC_UNUSED   = 0,                  /* slot free */
	PROC_EMBRYO,                        /* allocated, not yet runnable */
	PROC_READY,                         /* runnable, waiting for CPU */
	PROC_RUNNING,                       /* on the CPU now */
	PROC_SLEEPING,                      /* blocked on an event */
	PROC_ZOMBIE                         /* exited, waiting for parent reap */
};

struct proc {
	int               pid;              /* 1..PROC_MAX, or 0 if UNUSED */
	int               parent_pid;       /* 0 = no parent (boot proc) */
	enum proc_state   state;
	int               exit_code;        /* meaningful only when ZOMBIE */
	unsigned int      kstack_top;       /* top of this proc's kernel stack */
	unsigned int      esp;              /* saved kernel-stack pointer on switch */
	unsigned int      cr3;              /* page-directory phys addr (0 = shared) */
	char              name[PROC_NAME_LEN];
};

/* Initialise the process table and reserve PID 1 for the kernel boot process,
 * which is what every existing kernel path is implicitly running as today. */
void           proc_init(void);

/* Reserve a free slot, mark it EMBRYO, copy `name`, return the new struct.
 * Returns NULL if the table is full. Parent PID is taken from proc_current(). */
struct proc   *proc_alloc(const char *name);

/* Mark the slot UNUSED. Safe to call on any state. */
void           proc_free(struct proc *p);

/* Currently-running process. Always non-NULL after proc_init(); during early
 * boot, before proc_init() has been called, returns NULL so callers can
 * detect the pre-init phase and fall back to legacy single-process behaviour. */
struct proc   *proc_current(void);

/* Lookup by PID. Returns NULL if `pid` is out of range or UNUSED. */
struct proc   *proc_get(int pid);

/* Number of non-UNUSED slots. */
int            proc_count(void);

/* ---- Context switch primitives ---- */

/* Cooperative switch between two kernel stacks. Stores the current ESP into
 * *old_esp_ptr (after pushing callee-saved regs + EFLAGS) and reloads ESP
 * from new_esp; pops the matching frame and returns. Implemented in asm
 * (boot/context_asm.s). */
void           context_switch(unsigned int *old_esp_ptr, unsigned int new_esp);

/* Prepare a fresh kernel stack so the first context_switch into it transfers
 * control to `entry` with a 0 argument and a sane EFLAGS. `kstack_top` is the
 * highest address of the stack region (exclusive); returns the ESP value to
 * store in the PCB. The stack region must have room for at least 32 bytes
 * (5 callee-saved-reg slots + return address + small alignment slack). */
unsigned int   context_init(unsigned int kstack_top, void (*entry)(void));

/* Spawn a kernel thread: allocate a PCB slot + a 4 KiB kernel stack, pre-build
 * the stack via context_init() so the first switch jumps into `entry`, and
 * leave the new proc in PROC_READY. Returns the new PCB or NULL if the table
 * is full or out of memory. The scheduler (added in a follow-up commit) is
 * what eventually picks it up; until then the thread just sits READY. */
struct proc   *proc_kthread_create(const char *name, void (*entry)(void));

#endif
