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
#include "vfs.h"          /* struct vfs_fd + VFS_FD_MAX */

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

#define PROC_NSIG       32              /* matches POSIX signal-number range */

struct proc {
	int               pid;              /* 1..PROC_MAX, or 0 if UNUSED */
	int               parent_pid;       /* 0 = no parent (boot proc) */
	enum proc_state   state;
	int               exit_code;        /* meaningful only when ZOMBIE */
	unsigned int      kstack_top;       /* top of this proc's kernel stack */
	unsigned int      esp;              /* saved kernel-stack pointer on switch */
	unsigned int      cr3;              /* page-directory phys addr (0 = shared) */
	unsigned int      pending_signals;  /* bitmap, bit N = signal N pending */
	unsigned int      sig_handlers[PROC_NSIG];   /* userland handler addrs */
	struct vfs_fd     fd_table[VFS_FD_MAX];       /* per-process open files */
	int               priority;          /* 0 = highest .. 31 = lowest; default 16 */
	char              name[PROC_NAME_LEN];

	/* Blocking. A SLEEPING proc is skipped by the scheduler until
	 * proc_wake() (an event: child exit, kill) or, when sleep_timed is
	 * set, until the PIT reaches wake_tick. */
	unsigned int      wake_tick;
	int               sleep_timed;
	int               suspended;         /* SYS_SUSPEND: not schedulable until resumed */
	int               killed;            /* SYS_KILL: exits at its next safe point */

	/* Address-space state that is not in the page tables themselves:
	 * the program break and whether the framebuffer PDE is mapped. Saved
	 * and restored around every context switch (see paging_vm_save). */
	unsigned int      vm_heap_next;
	int               vm_fb_mapped;

	unsigned int      kstack_base;       /* lowest address of the kernel stack (canary lives here) */
	unsigned int      kstack_warned;     /* highest stack usage already reported on serial */
	unsigned int      pipe_ends;         /* bit h set = this proc holds pipe handle h (pipe.c) */
	unsigned int      last_run;          /* scheduler sequence number of its last turn on the CPU */

	/* x87 state (fnsave image, 108 bytes) while the process is not on the
	 * CPU. The kernel itself never uses floating point, so the state is
	 * swapped only in proc_yield. fpu_valid == 0 means "never ran with an
	 * FPU state of its own yet": it gets a freshly initialised one. */
	unsigned char     fpu[108] __attribute__((aligned(4)));
	int               fpu_valid;
};

/* Kernel stack size for every process but the boot one (which runs on the
 * static stack in gdt.c). Stacks come from a static pool indexed by PCB
 * slot, so they are never fragmented and never share a frame with user
 * data. */
#define KSTACK_SIZE     8192u

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
 * is full or out of memory. */
struct proc   *proc_kthread_create(const char *name, void (*entry)(void));

/* ---- Scheduling model ----
 *
 * The kernel does not preempt itself. A context switch happens only
 *   (a) when the PIT interrupts ring-3 code (trap.c), or
 *   (b) at an explicit proc_yield() / proc_sleep_ticks() / proc_block().
 * Code running in the kernel between two such points therefore sees every
 * kernel data structure in a consistent state without locks; interrupt
 * handlers are the only other writers and must stick to their own small
 * buffers. A syscall that has to wait must use the calls below instead of
 * spinning on hlt, or nothing else gets to run. */

/* Hand the CPU to the next runnable proc. Returns 1 if another proc ran in
 * between, 0 if nothing else was runnable (the caller keeps the CPU). */
int            proc_yield(void);

/* Block the caller for `ticks` PIT ticks (10 ms each), or until it is
 * woken early by proc_wake(). Other procs run meanwhile; with none
 * runnable the CPU idles in hlt. */
void           proc_sleep_ticks(unsigned int ticks);

/* Block the caller until proc_wake(), with a `max_ticks` safety timeout
 * (0 = no timeout). The caller re-checks its condition afterwards. */
void           proc_block(unsigned int max_ticks);

/* Make a SLEEPING proc runnable again. Safe from IRQ context. */
void           proc_wake(struct proc *p);

/* Called by the PIT handler every tick: wakes timed sleepers that are due. */
void           proc_tick(unsigned int now);

/* A tick that interrupts kernel code cannot switch processes, so it leaves
 * a note instead (proc_request_resched, from the PIT handler). The trap
 * exit path calls proc_resched_point() just before returning to ring 3 and
 * the switch happens there. Without this a process that spends most of its
 * time inside syscalls would never be preempted. */
void           proc_request_resched(void);
void           proc_resched_point(void);

/* Check the running proc's kernel stack: halt with a message if its canary
 * is gone, and log (once per new high) when more than three quarters of it
 * have ever been used. Read-only, callable from the PIT handler. */
void           proc_kstack_audit(void);

/* Give up the CPU if anyone else can use it, otherwise halt until the next
 * interrupt. For kernel polling loops (the menu's idle loop). */
void           proc_idle(void);

/* Ask `p` to die: it exits by itself at its next return to ring 3 with
 * exit code 137. Wakes it if it is blocked, un-suspends it. Returns 0, or
 * -1 for a proc that cannot be killed (PID 1, a zombie, an embryo). */
int            proc_kill(struct proc *p);

/* Turn the caller into a ZOMBIE and never return: hands zombie children to
 * the reaper, orphans living ones, wakes a parent blocked in wait. The
 * caller must already have released its own resources (sockets, audio). */
void           proc_exit_current(void) __attribute__((noreturn));

/* Reset the running process's FPU to the power-on state (a new program
 * image must not inherit its predecessor's registers). */
void           proc_fpu_reset(void);

/* Name a process after the program it runs ("SNAKE.ELF" -> "SNAKE").
 * fork copies the parent's name; without this every program started from
 * the desktop or the shell would show up as its launcher. */
void           proc_set_name(struct proc *p, const char *elf_name);

/* A process just replaced its program image (exec, or the menu launching
 * an app in PID 1): anything that referred to the old image is void.
 * Clears signal handlers (their addresses belonged to the old code) and
 * pending signals; for the foreground process it also discards queued
 * keyboard and mouse input, so keys typed at the previous program are not
 * replayed into the new one. */
void           proc_reset_image_state(struct proc *p);

/* Detach every child of `p` (reap the zombies, orphan the living) without
 * exiting. Used when the boot proc's app ends and it becomes the menu
 * again: nobody would ever wait for those children. */
void           proc_abandon_children(struct proc *p);

/* Free every ZOMBIE that has no parent left to wait for it. Never touches
 * the caller. Process context only. */
void           proc_reap_orphans(void);

/* Release a ZOMBIE proc: destroy its address space, mark the slot UNUSED.
 * Returns the proc's exit_code. Safe to call only on a ZOMBIE that is not
 * the caller; returns -1 otherwise. */
int            proc_reap(struct proc *p);

/* Find a ZOMBIE child of the caller. If `pid` > 0, return that PID only when
 * it is a ZOMBIE child of the caller; if `pid` <= 0, return any ZOMBIE child.
 * Returns NULL if no matching ZOMBIE is currently present (caller may have
 * living children that have not yet exited). */
struct proc   *proc_find_zombie_child(int pid);

/* True if the caller has any non-UNUSED non-ZOMBIE child, i.e. there is still
 * a child that might exit in the future. Used by SYS_WAIT to distinguish
 * "nothing to wait for" from "waiting on a live child". */
int            proc_has_living_child(void);

/* Fill `out` with a human-readable snapshot of every non-UNUSED PCB:
 *   "PID  STATE     PRIO  PARENT  NAME\n" per row, with a one-line header.
 * Returns the number of bytes written (excluding NUL), truncated to `cap`.
 * Backs SYS_PROC_INFO and the shell's `ps` command. */
unsigned int   proc_snapshot(char *out, unsigned int cap);

#endif
