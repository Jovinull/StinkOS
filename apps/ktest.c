/* ktest: kernel regression cases run from ring 3 against the real kernel.
 *
 * Each case targets one invariant the kernel must hold whatever userland
 * does: no frame leaks across process lifetimes, per-process heaps, COW
 * isolation that survives kernel-side writes, pointer validation that
 * matches what is actually mapped, a scheduler that neither starves nor
 * burns CPU on sleepers.
 *
 * The case to run comes from the KTEST.ARG file ("all" when missing), so
 * tools/qa/ktest.py can boot one VM per case: a case that takes the kernel
 * down cannot hide the others. Results go to the serial log:
 *
 *   ktest: <case>: PASS
 *   ktest: <case>: FAIL <why>
 *   ktest: done
 *
 * and the VM powers itself off, which is what the harness waits for.
 */
#include "libstink.h"

#define PAGE 4096u

static char failmsg[160];
#define FAIL(...) do { snprintf(failmsg, sizeof(failmsg), __VA_ARGS__); return 1; } while (0)

static unsigned int free_pages(void)
{
	struct sys_meminfo m;
	if (sys_meminfo(&m) != 0)
		return 0;
	return m.free_pages;
}

/* Rows in the process table (one line per PCB, plus the header). */
static int proc_rows(void)
{
	static char buf[1024];
	int n = sys_proc_info(buf, sizeof(buf));
	int rows = 0;
	for (int i = 0; i < n; i++)
		if (buf[i] == '\n')
			rows++;
	return rows - 1;
}

static void child_exit(int code)
{
	sys_exit_code(code);
	for (;;)
		;
}

/* ---- memory lifetime ----------------------------------------------------- */

/* A fork()ed child that exits must give every frame back once reaped. */
static int t_leak_fork(void)
{
	for (int warm = 0; warm < 2; warm++) {
		int pid = sys_fork();
		if (pid < 0) FAIL("warm-up fork failed");
		if (pid == 0) child_exit(7);
		sys_waitpid(pid);
	}
	unsigned int base = free_pages();
	for (int i = 0; i < 30; i++) {
		int pid = sys_fork();
		if (pid < 0) FAIL("fork %d failed (free=%u, base=%u)", i, free_pages(), base);
		if (pid == 0) child_exit(7);
		int code = sys_waitpid(pid);
		if (code != 7) FAIL("child %d exit code %d, want 7", i, code);
	}
	unsigned int after = free_pages();
	if (after != base) FAIL("leaked %d frames over 30 fork+exit cycles", (int)(base - after));
	return 0;
}

/* Same, but the child replaces its image first. */
static int t_leak_exec(void)
{
	for (int warm = 0; warm < 2; warm++) {
		int pid = sys_fork();
		if (pid < 0) FAIL("warm-up fork failed");
		if (pid == 0) { sys_exec("ktchild"); child_exit(99); }
		sys_waitpid(pid);
	}
	unsigned int base = free_pages();
	for (int i = 0; i < 20; i++) {
		int pid = sys_fork();
		if (pid < 0) FAIL("fork %d failed (free=%u, base=%u)", i, free_pages(), base);
		if (pid == 0) { sys_exec("ktchild"); child_exit(99); }
		int code = sys_waitpid(pid);
		if (code != 42) FAIL("child %d exit code %d, want 42", i, code);
	}
	unsigned int after = free_pages();
	if (after != base) FAIL("leaked %d frames over 20 fork+exec cycles", (int)(base - after));
	return 0;
}

/* Releasing a large mapping must return all of it to the allocator. */
static int t_pmm_churn(void)
{
	unsigned int size = 8u * 1024 * 1024;
	unsigned int base = free_pages();
	unsigned char *p = sys_mmap(size);
	if (!p) FAIL("mmap(8 MiB) failed with %u pages free", base);
	for (unsigned int off = 0; off < size; off += PAGE)
		p[off] = 1;
	if (free_pages() + size / PAGE > base)
		FAIL("mapping 2048 pages only consumed %d", (int)(base - free_pages()));
	if (sys_munmap(p, size) != 0) FAIL("munmap failed");
	unsigned int after = free_pages();
	if (after != base) FAIL("lost %d frames after munmap of 2048 pages", (int)(base - after));
	return 0;
}

/* ---- per-process address space ------------------------------------------ */

/* The program break belongs to the process: a child exec'ing and growing
 * its own heap must not move ours, and our heap must stay usable. */
static int t_heap_switch(void)
{
	unsigned int n = 64 * 1024;
	unsigned char *h = sys_sbrk((int)n);
	if (h == (void *)-1) FAIL("sbrk failed");
	for (unsigned int i = 0; i < n; i++)
		h[i] = (unsigned char)(i * 7 + 3);
	void *brk0 = sys_sbrk(0);

	int pid = sys_fork();
	if (pid < 0) FAIL("fork failed");
	if (pid == 0) { sys_exec("ktchild"); child_exit(99); }
	int code = sys_waitpid(pid);
	if (code != 42) FAIL("child exit code %d, want 42", code);

	void *brk1 = sys_sbrk(0);
	if (brk1 != brk0) FAIL("break moved from %p to %p after child exec", brk0, brk1);
	if (sys_fwrite("KT.TMP", h + n - PAGE, PAGE) != 0)
		FAIL("kernel rejected a buffer inside our own heap");
	unsigned char *h2 = sys_sbrk((int)PAGE);
	if (h2 != brk0) FAIL("sbrk returned %p, want %p", (void *)h2, brk0);
	h2[0] = 0xEE;
	for (unsigned int i = 0; i < n; i++)
		if (h[i] != (unsigned char)(i * 7 + 3))
			FAIL("heap byte %u corrupted", i);
	sys_fdelete("KT.TMP");
	return 0;
}

static unsigned char shared_page[2 * PAGE] __attribute__((aligned(4096)));

/* A kernel write into a page that is still COW-shared must privatize it,
 * exactly like a ring-3 write would. */
static int t_cow_kwrite(void)
{
	static unsigned char pat[PAGE];
	memset(pat, 0x55, sizeof(pat));
	if (sys_fwrite("KT.DAT", pat, sizeof(pat)) != 0) FAIL("could not create KT.DAT");
	memset(shared_page, 0xAA, sizeof(shared_page));

	int pid = sys_fork();
	if (pid < 0) FAIL("fork failed");
	if (pid == 0) {
		int r = sys_fread("KT.DAT", shared_page, PAGE);
		child_exit((r == (int)PAGE && shared_page[0] == 0x55 &&
		            shared_page[PAGE - 1] == 0x55) ? 1 : 2);
	}
	int code = sys_waitpid(pid);
	sys_fdelete("KT.DAT");
	if (code != 1) FAIL("child read failed (exit %d)", code);
	for (unsigned int i = 0; i < sizeof(shared_page); i++)
		if (shared_page[i] != 0xAA)
			FAIL("child's fread landed in the parent's page (byte %u = %x)",
			     i, shared_page[i]);
	return 0;
}

/* Never called: just bytes in .text for the next case to aim at. */
__attribute__((noinline, used)) static int victim(int x)
{
	volatile int acc = x;
	for (int i = 0; i < 64; i++)
		acc = acc * 31 + i;
	return acc;
}

/* .text is mapped read-only; the kernel must refuse to write there on a
 * process's behalf (the frames are shared with every fork of the image). */
static int t_text_kwrite(void)
{
	static unsigned char pat[64];
	memset(pat, 0x55, sizeof(pat));
	if (sys_fwrite("KT.DAT", pat, sizeof(pat)) != 0) FAIL("could not create KT.DAT");
	unsigned char *text = (unsigned char *)victim;
	unsigned char before[32];
	memcpy(before, text, sizeof(before));

	int pid = sys_fork();
	if (pid < 0) FAIL("fork failed");
	if (pid == 0) {
		int r = sys_fread("KT.DAT", text, 32);
		child_exit(r == -1 ? 1 : 2);
	}
	int code = sys_waitpid(pid);
	sys_fdelete("KT.DAT");
	if (memcmp(before, text, sizeof(before)) != 0)
		FAIL("kernel overwrote read-only .text shared with the parent");
	if (code != 1) FAIL("fread into .text was not rejected (child exit %d)", code);
	return 0;
}

/* A pointer into an unmapped hole must be rejected, not dereferenced. */
static int t_munmap_hole(void)
{
	static unsigned char pat[128];
	if (sys_fwrite("KT.DAT", pat, sizeof(pat)) != 0) FAIL("could not create KT.DAT");
	unsigned char *p = sys_mmap(2 * PAGE);
	if (!p) FAIL("mmap failed");
	p[0] = 1; p[PAGE] = 1;
	if (sys_munmap(p, PAGE) != 0) FAIL("munmap failed");
	int r = sys_fread("KT.DAT", p, 100);
	if (r != -1) FAIL("fread into an unmapped page returned %d", r);
	r = sys_fwrite("KT.TMP", p, 100);
	if (r != -1) FAIL("fwrite from an unmapped page returned %d", r);
	r = sys_fread("KT.DAT", p + PAGE - 50, 100);   /* straddles the hole */
	if (r != -1) FAIL("fread straddling the hole returned %d", r);
	sys_fdelete("KT.DAT");
	sys_fdelete("KT.TMP");
	return 0;
}

/* ---- process lifetime ---------------------------------------------------- */

static int t_kill_reap(void)
{
	int rows0 = proc_rows();
	for (int warm = 0; warm < 2; warm++) {
		int pid = sys_fork();
		if (pid < 0) FAIL("warm-up fork failed");
		if (pid == 0) child_exit(0);
		sys_waitpid(pid);
	}
	unsigned int base = free_pages();
	for (int i = 0; i < 5; i++) {
		int pid = sys_fork();
		if (pid < 0) FAIL("fork failed");
		if (pid == 0) {
			for (;;)
				sys_sleep_ms(10);
		}
		sys_sleep_ms(30);
		if (sys_kill(pid) != 0) FAIL("kill(%d) failed", pid);
		int code = sys_waitpid(pid);
		if (code != 137) FAIL("killed child exit code %d, want 137", code);
	}
	if (proc_rows() != rows0) FAIL("process table has %d rows, want %d", proc_rows(), rows0);
	unsigned int after = free_pages();
	if (after != base) FAIL("leaked %d frames over 5 kill+reap cycles", (int)(base - after));
	return 0;
}

/* A grandchild whose parent is gone has nobody to wait for it: the kernel
 * has to reap it by itself. */
static int t_orphan(void)
{
	int rows0 = proc_rows();
	for (int warm = 0; warm < 2; warm++) {
		int pid = sys_fork();
		if (pid < 0) FAIL("warm-up fork failed");
		if (pid == 0) child_exit(0);
		sys_waitpid(pid);
	}
	unsigned int base = free_pages();
	for (int i = 0; i < 4; i++) {
		int pid = sys_fork();
		if (pid < 0) FAIL("fork failed");
		if (pid == 0) {
			int gp = sys_fork();
			if (gp == 0) {
				sys_sleep_ms(60);
				child_exit(3);
			}
			child_exit(gp > 0 ? 2 : 9);
		}
		int code = sys_waitpid(pid);
		if (code != 2) FAIL("child exit code %d, want 2", code);
	}
	sys_sleep_ms(400);
	if (proc_rows() != rows0)
		FAIL("%d orphaned zombies still in the process table", proc_rows() - rows0);
	unsigned int after = free_pages();
	if (after != base) FAIL("leaked %d frames over 4 orphan cycles", (int)(base - after));
	return 0;
}

/* ---- scheduling ---------------------------------------------------------- */

static unsigned int spin_for(unsigned int nticks)
{
	unsigned int count = 0;
	unsigned int t0 = sys_ticks();
	while (sys_ticks() == t0)
		;                                   /* align to a tick edge */
	t0 = sys_ticks();
	while ((unsigned int)(sys_ticks() - t0) < nticks)
		count++;
	return count;
}

/* Sleeping processes must not eat the time slices of a runnable one. */
static int t_sleep_fair(void)
{
	unsigned int alone = spin_for(30);
	int pids[4];
	for (int i = 0; i < 4; i++) {
		pids[i] = sys_fork();
		if (pids[i] < 0) FAIL("fork failed");
		if (pids[i] == 0)
			for (;;)
				sys_sleep_ms(1000);
	}
	sys_sleep_ms(50);
	unsigned int crowded = spin_for(30);
	for (int i = 0; i < 4; i++) {
		sys_kill(pids[i]);
		sys_waitpid(pids[i]);
	}
	unsigned int pct = alone ? crowded * 100u / alone : 0;
	if (pct < 70)
		FAIL("4 sleeping processes cut a busy one to %u%% of its CPU", pct);
	return 0;
}

/* Two CPU-bound processes must both make progress. */
static int t_preempt(void)
{
	int pid = sys_fork();
	if (pid < 0) FAIL("fork failed");
	if (pid == 0) {
		unsigned int c = spin_for(40);
		child_exit(c > 0 ? 1 : 2);
	}
	unsigned int mine = spin_for(40);
	int code = sys_waitpid(pid);
	if (code != 1) FAIL("child made no progress (exit %d)", code);
	if (mine == 0) FAIL("parent made no progress");
	return 0;
}

/* A process that lives almost entirely inside syscalls must still be
 * preempted: timer ticks that land in kernel mode have to turn into a
 * switch on the way back to ring 3. */
static int t_resched(void)
{
	sys_fdelete("KTR.TMP");
	int pid = sys_fork();
	if (pid < 0) FAIL("fork failed");
	if (pid == 0) {
		char one = 'x';
		child_exit(sys_fwrite("KTR.TMP", &one, 1) == 0 ? 1 : 2);
	}
	/* Poll with back-to-back syscalls and no sleep: the child only gets
	 * the CPU if the kernel reschedules us at a syscall boundary. */
	unsigned int t0 = sys_ticks();
	int seen = 0;
	char b;
	while ((unsigned int)(sys_ticks() - t0) < 100) {
		if (sys_fread("KTR.TMP", &b, 1) == 1) {
			seen = 1;
			break;
		}
	}
	unsigned int waited = sys_ticks() - t0;
	int code = sys_waitpid(pid);
	sys_fdelete("KTR.TMP");
	if (!seen) FAIL("child never ran while the parent spun in syscalls for 1 s");
	if (code != 1) FAIL("child exit code %d, want 1", code);
	if (waited > 20) FAIL("child needed %u ticks to get scheduled", waited);
	return 0;
}

/* Two processes hammering the filesystem and the frame allocator at once:
 * every byte each one reads back must be its own, and no frame may go
 * missing. */
static int hammer(const char *name, unsigned char seed, int rounds)
{
	static unsigned char wr[2048], rd[2048];
	for (int r = 0; r < rounds; r++) {
		for (unsigned int i = 0; i < sizeof(wr); i++)
			wr[i] = (unsigned char)(seed + r + i);
		if (sys_fwrite(name, wr, sizeof(wr)) != 0)
			return 10;
		unsigned char *m = sys_mmap(16 * PAGE);
		if (!m)
			return 11;
		for (unsigned int i = 0; i < 16 * PAGE; i += PAGE)
			m[i] = seed;
		memset(rd, 0, sizeof(rd));
		if (sys_fread(name, rd, sizeof(rd)) != (int)sizeof(rd))
			return 12;
		if (memcmp(wr, rd, sizeof(wr)) != 0)
			return 13;
		for (unsigned int i = 0; i < 16 * PAGE; i += PAGE)
			if (m[i] != seed)
				return 14;
		if (sys_munmap(m, 16 * PAGE) != 0)
			return 15;
	}
	return 0;
}

static int t_concurrent(void)
{
	for (int warm = 0; warm < 2; warm++) {
		int pid = sys_fork();
		if (pid < 0) FAIL("warm-up fork failed");
		if (pid == 0) child_exit(0);
		sys_waitpid(pid);
	}
	/* Both sides leave their mmap pages above the break; do it once here so
	 * the page tables backing that range already exist in the baseline. */
	unsigned int base = free_pages();
	int pid = sys_fork();
	if (pid < 0) FAIL("fork failed");
	if (pid == 0)
		child_exit(hammer("KTB.TMP", 0x40, 150));
	int mine = hammer("KTA.TMP", 0x90, 150);
	int theirs = sys_waitpid(pid);
	sys_fdelete("KTA.TMP");
	sys_fdelete("KTB.TMP");
	if (mine != 0) FAIL("parent saw corruption (step %d)", mine);
	if (theirs != 0) FAIL("child saw corruption (step %d)", theirs);
	unsigned int after = free_pages();
	if (after != base) FAIL("lost %d frames", (int)(base - after));
	return 0;
}

/* ---- hostile arguments --------------------------------------------------- */

/* Syscalls that take user pointers / lengths and are safe to call with
 * garbage (nothing here writes a disk sector outside StinkFS, powers the
 * machine off or blocks). */
static const unsigned char fuzz_calls[] = {
	1, 8, 9, 11, 12, 13, 14, 15, 16, 18, 19, 21, 26, 27, 28, 32, 33, 36, 37,
	39, 41, 43, 49, 50, 51, 57, 58, 59, 63, 64, 67, 68, 73, 74,
};

static int t_badptr(void)
{
	unsigned char *hole = sys_mmap(PAGE);
	if (!hole) FAIL("mmap failed");
	sys_munmap(hole, PAGE);
	unsigned int heap_end = (unsigned int)sys_sbrk(0);
	const unsigned int ptrs[] = {
		0u, 1u, 0x1000u, 0x3FFFFFu, 0x400000u, 0x53FFFFu, 0x540000u,
		(unsigned int)hole, heap_end, heap_end - 1u, 0x13FFFFFu, 0x1400000u,
		0x10000000u, 0x12000000u, 0x7FFFFFFFu, 0x80000000u, 0x80100000u,
		0xC0000000u, 0xFFFFF000u, 0xFFFFFFFFu,
	};
	const unsigned int lens[] = {
		0u, 1u, 16u, 4096u, 0x100000u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu,
	};
	unsigned int calls = 0;
	for (unsigned int c = 0; c < sizeof(fuzz_calls); c++) {
		int n = fuzz_calls[c];
		for (unsigned int p = 0; p < sizeof(ptrs) / sizeof(ptrs[0]); p++) {
			for (unsigned int l = 0; l < sizeof(lens) / sizeof(lens[0]); l++) {
				int P = (int)ptrs[p], L = (int)lens[l];
				__syscall4(n, P, L, P, L);
				__syscall4(n, L, P, L, P);
				__syscall4(n, P, P, L, L);
				__syscall4(n, 3, P, L, 0);
				calls += 4;
			}
		}
	}
	/* Still alive and still sane? */
	struct sys_meminfo m;
	if (sys_meminfo(&m) != 0 || m.free_pages == 0)
		FAIL("kernel state looks wrong after %u hostile calls", calls);
	sys_printf("ktest: badptr: survived %u calls", calls);
	return 0;
}

/* Ring-3 EFLAGS must not steer kernel code. With DF set, an unguarded
 * `rep movs` in the kernel copies backwards (fork's trap-frame clone, the
 * directory compaction in delete); with NT set, a kernel `iret` attempts a
 * task switch. Both used to take the machine down. */
static int t_eflags(void)
{
	int pid = sys_fork();
	if (pid < 0) FAIL("fork failed");
	if (pid == 0) {
		/* DF=1 across fork and a file create/delete. */
		__asm__ volatile ("std");
		int gp = sys_fork();
		__asm__ volatile ("cld");
		if (gp == 0) child_exit(5);
		if (gp < 0 || sys_waitpid(gp) != 5) child_exit(10);

		static unsigned char blob[1500];
		for (unsigned int i = 0; i < sizeof(blob); i++)
			blob[i] = (unsigned char)(i * 3);
		sys_fwrite("KTE1.TMP", blob, sizeof(blob));
		sys_fwrite("KTE2.TMP", blob, sizeof(blob));
		__asm__ volatile ("std");
		int d = sys_fdelete("KTE1.TMP");
		__asm__ volatile ("cld");
		static unsigned char back[1500];
		int n = sys_fread("KTE2.TMP", back, sizeof(back));
		sys_fdelete("KTE2.TMP");
		if (d != 0 || n != (int)sizeof(back) || memcmp(blob, back, sizeof(blob)) != 0)
			child_exit(11);

		/* NT=1 across fork. */
		__asm__ volatile ("pushf; orl $0x4000, (%esp); popf");
		gp = sys_fork();
		__asm__ volatile ("pushf; andl $0xFFFFBFFF, (%esp); popf");
		if (gp == 0) child_exit(6);
		if (gp < 0 || sys_waitpid(gp) != 6) child_exit(12);
		child_exit(1);
	}
	int code = sys_waitpid(pid);
	if (code != 1) FAIL("child reported step %d", code);
	return 0;
}

/* A failed sbrk must not keep the frames it grabbed on the way. */
static int t_sbrk_oom(void)
{
	unsigned int base = free_pages();
	void *brk0 = sys_sbrk(0);
	void *r = sys_sbrk(0x7FF00000);
	if (r != (void *)-1) FAIL("absurd sbrk succeeded");
	if (sys_sbrk(0) != brk0) FAIL("break moved after a failed sbrk");
	unsigned int after = free_pages();
	if (after != base) FAIL("failed sbrk kept %d frames", (int)(base - after));
	return 0;
}

/* ---- driver ---------------------------------------------------------------- */

struct ktcase {
	const char *name;
	int (*fn)(void);
};

static const struct ktcase cases[] = {
	{ "leak-fork",   t_leak_fork },
	{ "leak-exec",   t_leak_exec },
	{ "pmm-churn",   t_pmm_churn },
	{ "heap-switch", t_heap_switch },
	{ "cow-kwrite",  t_cow_kwrite },
	{ "text-kwrite", t_text_kwrite },
	{ "munmap-hole", t_munmap_hole },
	{ "kill-reap",   t_kill_reap },
	{ "orphan",      t_orphan },
	{ "sleep-fair",  t_sleep_fair },
	{ "preempt",     t_preempt },
	{ "resched",     t_resched },
	{ "concurrent",  t_concurrent },
	{ "sbrk-oom",    t_sbrk_oom },
	{ "eflags",      t_eflags },
	{ "badptr",      t_badptr },
};

void main(void)
{
	char arg[32];
	int n = sys_fread("KTEST.ARG", arg, sizeof(arg) - 1);
	if (n < 0)
		n = 0;
	while (n > 0 && arg[n - 1] <= ' ')
		n--;
	arg[n] = '\0';
	int all = (n == 0 || strcmp(arg, "all") == 0);

	int ran = 0;
	for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		if (!all && strcmp(arg, cases[i].name) != 0)
			continue;
		ran++;
		sys_printf("ktest: %s: start", cases[i].name);
		failmsg[0] = '\0';
		if (cases[i].fn() == 0)
			sys_printf("ktest: %s: PASS", cases[i].name);
		else
			sys_printf("ktest: %s: FAIL %s", cases[i].name, failmsg);
	}
	if (!ran)
		sys_printf("ktest: no such case '%s'", arg);
	sys_log("ktest: done");
	sys_shutdown();
}
