/* Host-side test for the ZOMBIE / REAP path in kernel/sys/proc.c.
 *
 *   - proc_reap: only succeeds on PROC_ZOMBIE slots, returns the
 *     exit code, frees the kstack, marks the slot UNUSED. Any other
 *     state -> -1 (no side effect).
 *
 *   - proc_find_zombie_child: scans for ZOMBIE slots parented by the
 *     caller's PID. If pid > 0, must match that PID too. Returns the
 *     first hit or NULL.
 *
 *   - proc_has_living_child: returns 1 if any slot parented by the
 *     caller is in any state other than UNUSED or ZOMBIE.
 *
 * The "ZOMBIE counts as not-living" bit is the load-bearing part of
 * sys_wait: if a regression flips it, a parent that already collected
 * a zombie via SYS_WAITPID would still see `has_living_child` == 1
 * and block forever on the next wait.
 */
#include <stdio.h>

#define PROC_MAX 8

enum proc_state {
	PROC_UNUSED = 0,
	PROC_EMBRYO,
	PROC_READY,
	PROC_RUNNING,
	PROC_SLEEPING,
	PROC_ZOMBIE,
};

struct proc {
	int             pid;
	int             parent_pid;
	enum proc_state state;
	int             exit_code;
	unsigned int    kstack_top;
};

static struct proc table[PROC_MAX];
static struct proc *running;
static int frames_freed_count;

static void pmm_free_stub(unsigned int frame) { (void)frame; frames_freed_count++; }

static void proc_free_sim(struct proc *p)
{
	p->state = PROC_UNUSED;
	p->pid = 0;
	p->parent_pid = 0;
	p->kstack_top = 0;
}

static int proc_reap(struct proc *p)
{
	if (!p || p->state != PROC_ZOMBIE) return -1;
	int code = p->exit_code;
	if (p->kstack_top) pmm_free_stub(p->kstack_top - 4096);
	proc_free_sim(p);
	return code;
}

static struct proc *proc_find_zombie_child(int pid)
{
	if (!running) return 0;
	for (int i = 0; i < PROC_MAX; i++) {
		struct proc *p = &table[i];
		if (p->state != PROC_ZOMBIE) continue;
		if (p->parent_pid != running->pid) continue;
		if (pid > 0 && p->pid != pid) continue;
		return p;
	}
	return 0;
}

static int proc_has_living_child(void)
{
	if (!running) return 0;
	for (int i = 0; i < PROC_MAX; i++) {
		struct proc *p = &table[i];
		if (p->state == PROC_UNUSED || p->state == PROC_ZOMBIE) continue;
		if (p->parent_pid == running->pid) return 1;
	}
	return 0;
}

static void table_reset(void)
{
	for (int i = 0; i < PROC_MAX; i++) {
		table[i].state = PROC_UNUSED;
		table[i].pid = 0;
		table[i].parent_pid = 0;
		table[i].exit_code = 0;
		table[i].kstack_top = 0;
	}
	frames_freed_count = 0;
	running = 0;
}

static struct proc *spawn(int pid, int parent, enum proc_state st)
{
	for (int i = 0; i < PROC_MAX; i++) {
		if (table[i].state == PROC_UNUSED) {
			table[i].pid = pid;
			table[i].parent_pid = parent;
			table[i].state = st;
			table[i].exit_code = 0;
			table[i].kstack_top = 0x10000 + (unsigned)i * 0x1000;
			return &table[i];
		}
	}
	return 0;
}

static int expect_int(const char *label, int got, int want)
{
	if (got == want) { printf("ok   %-55s = %d\n", label, got); return 0; }
	printf("FAIL %s: got %d, want %d\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;
	struct proc *p, *child;

	/* --- proc_reap NULL / non-zombie rejected --------------- */
	table_reset();
	failures += expect_int("reap NULL -> -1", proc_reap(0), -1);

	p = spawn(2, 1, PROC_READY);
	failures += expect_int("reap READY -> -1", proc_reap(p), -1);

	p->state = PROC_RUNNING;
	failures += expect_int("reap RUNNING -> -1", proc_reap(p), -1);

	p->state = PROC_SLEEPING;
	failures += expect_int("reap SLEEPING -> -1", proc_reap(p), -1);

	p->state = PROC_EMBRYO;
	failures += expect_int("reap EMBRYO -> -1", proc_reap(p), -1);

	failures += expect_int("no frames freed by rejected reaps",
	                       frames_freed_count, 0);

	/* --- successful reap frees kstack and returns exit_code ---- */
	table_reset();
	p = spawn(3, 1, PROC_ZOMBIE);
	p->exit_code = 42;
	failures += expect_int("reap ZOMBIE -> exit_code 42", proc_reap(p), 42);
	failures += expect_int("post-reap slot UNUSED",
	                       (int)table[0].state, PROC_UNUSED);
	failures += expect_int("post-reap: 1 kstack frame freed",
	                       frames_freed_count, 1);

	/* --- proc_reap on already-reaped slot rejected -------- */
	failures += expect_int("re-reap same slot -> -1", proc_reap(p), -1);

	/* --- find_zombie_child without `running` -> NULL ----- */
	table_reset();
	(void)spawn(2, 1, PROC_ZOMBIE);
	failures += expect_int("find without running set -> NULL",
	                       proc_find_zombie_child(-1) != 0, 0);

	/* --- find_zombie_child wildcard (pid=-1) returns first match */
	table_reset();
	running = spawn(1, 0, PROC_RUNNING);
	(void)spawn(2, 1, PROC_READY);
	child = spawn(3, 1, PROC_ZOMBIE);
	failures += expect_int("wildcard find -> picks zombie child",
	                       proc_find_zombie_child(-1) == child, 1);

	/* --- specific pid lookup ----------------------------- */
	table_reset();
	running = spawn(1, 0, PROC_RUNNING);
	(void)spawn(2, 1, PROC_ZOMBIE);
	struct proc *c3 = spawn(3, 1, PROC_ZOMBIE);
	failures += expect_int("find pid=3 -> exact match",
	                       proc_find_zombie_child(3) == c3, 1);
	failures += expect_int("find pid=99 (no child) -> NULL",
	                       proc_find_zombie_child(99) != 0, 0);

	/* --- foreign zombie (parented by someone else) ignored */
	table_reset();
	running = spawn(1, 0, PROC_RUNNING);
	(void)spawn(2, 99, PROC_ZOMBIE);           /* zombie of an alien parent */
	failures += expect_int("find skips foreign zombie -> NULL",
	                       proc_find_zombie_child(-1) != 0, 0);

	/* --- has_living_child counts non-zombie/non-unused --- */
	table_reset();
	running = spawn(1, 0, PROC_RUNNING);
	failures += expect_int("no children -> 0", proc_has_living_child(), 0);

	(void)spawn(2, 1, PROC_READY);
	failures += expect_int("READY child -> 1", proc_has_living_child(), 1);

	table_reset();
	running = spawn(1, 0, PROC_RUNNING);
	(void)spawn(2, 1, PROC_SLEEPING);
	failures += expect_int("SLEEPING child -> 1", proc_has_living_child(), 1);

	table_reset();
	running = spawn(1, 0, PROC_RUNNING);
	(void)spawn(2, 1, PROC_EMBRYO);
	failures += expect_int("EMBRYO child -> 1", proc_has_living_child(), 1);

	/* CRITICAL: zombie children do NOT count as living. Otherwise
	 * sys_wait blocks even after the parent already collected the
	 * exit status. */
	table_reset();
	running = spawn(1, 0, PROC_RUNNING);
	(void)spawn(2, 1, PROC_ZOMBIE);
	failures += expect_int("ZOMBIE child does NOT count as living -> 0",
	                       proc_has_living_child(), 0);

	/* Mixed: one zombie + one ready -> 1 (the ready). */
	table_reset();
	running = spawn(1, 0, PROC_RUNNING);
	(void)spawn(2, 1, PROC_ZOMBIE);
	(void)spawn(3, 1, PROC_READY);
	failures += expect_int("zombie + ready child -> 1",
	                       proc_has_living_child(), 1);

	/* Foreign children ignored. */
	table_reset();
	running = spawn(1, 0, PROC_RUNNING);
	(void)spawn(2, 99, PROC_READY);
	failures += expect_int("foreign live child ignored -> 0",
	                       proc_has_living_child(), 0);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
