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

/* The process table as text: one header line, then one row per process,
 * and our own row says who we are. */
static int t_procinfo(void)
{
	static char buf[1024];
	int n = sys_proc_info(buf, sizeof(buf));
	if (n <= 0 || n >= (int)sizeof(buf)) FAIL("proc_info returned %d", n);
	buf[n] = '\0';
	if (strncmp(buf, "PID", 3) != 0) FAIL("no header line");
	if (proc_rows() < 1) FAIL("no process rows after the header");

	int me = sys_getpid(), seen_me = 0, rows = 0;
	const char *p = strchr(buf, '\n');
	while (p && p[1]) {
		p++;
		const char *digits = p;
		while (*digits == ' ')
			digits++;                       /* numbers are right-aligned in their column */
		int pid = atoi(digits);
		if (pid <= 0) FAIL("row %d does not start with a pid", rows);
		const char *eol = strchr(p, '\n');
		if (!eol) FAIL("row %d is not terminated", rows);
		if (eol - p > 60) FAIL("row %d is %d characters long", rows, (int)(eol - p));
		if (pid == me) {
			seen_me = 1;
			if (!strstr(p, "RUN")) FAIL("our own row is not RUN");
			static char name[16];
			if ((unsigned int)(eol - p) < 5) FAIL("row too short");
			memcpy(name, eol - 5, 5);
			name[5] = '\0';
			if (strcmp(name, "KTEST") != 0) FAIL("our row is named '%s', want KTEST", name);
		}
		rows++;
		p = eol;
	}
	if (!seen_me) FAIL("pid %d is missing from %d rows", me, rows);

	/* A child shows up as one more row, and goes away when reaped. */
	int pid = sys_fork();
	if (pid < 0) FAIL("fork failed");
	if (pid == 0) { sys_sleep_ms(200); child_exit(0); }
	if (proc_rows() != rows + 1) FAIL("a new child did not add a row");
	sys_waitpid(pid);
	if (proc_rows() != rows) FAIL("a reaped child still has a row");
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

/* Three CPU-bound processes must all make progress while the kernel's own
 * high-priority threads wake every tick (a window is open, so the
 * compositor runs; the NIC keeps the network thread busy). A round-robin
 * that restarts from the thread's table slot starves all but one of them. */
static int t_fair3(void)
{
	if (sys_win_create(200, 100, 0) != 0 || sys_win_show(-1, -1, "fair") != 0)
		FAIL("could not open a window");
	int pids[2];
	for (int i = 0; i < 2; i++) {
		pids[i] = sys_fork();
		if (pids[i] < 0) FAIL("fork failed");
		if (pids[i] == 0) {
			unsigned int c = spin_for(60);
			child_exit(c > 0 ? (int)(c / 256u) + 1 : 0);
		}
	}
	unsigned int mine = spin_for(60);
	unsigned int share[3] = { mine / 256u + 1, 0, 0 };
	for (int i = 0; i < 2; i++) {
		int code = sys_waitpid(pids[i]);
		if (code <= 0) FAIL("child %d made no progress", i);
		share[i + 1] = (unsigned int)code;
	}
	sys_win_destroy();
	unsigned int total = share[0] + share[1] + share[2];
	for (int i = 0; i < 3; i++)
		if (share[i] * 100u / total < 15u)
			FAIL("process %d got %u%% of the CPU (shares %u %u %u)", i,
			     share[i] * 100u / total, share[0], share[1], share[2]);
	return 0;
}

/* Floating-point registers are per-process state. Each worker parks two
 * values on the x87 stack, lets the scheduler run the other one for a few
 * ticks, and takes them back: they must be its own. */
static int fpu_worker(double seed, int rounds)
{
	for (int r = 0; r < rounds; r++) {
		double a = seed + r, b = (seed + r) * 3.0;
		double ra = 0, rb = 0;
		__asm__ volatile ("fldl %0\n\tfldl %1" : : "m"(a), "m"(b));
		unsigned int t0 = sys_ticks();
		while ((unsigned int)(sys_ticks() - t0) < 3)
			;                               /* integer-only: nothing here touches the FPU */
		__asm__ volatile ("fstpl %0\n\tfstpl %1" : "=m"(rb), "=m"(ra));
		if (ra != a || rb != b)
			return r + 1;
	}
	return 0;
}

static int t_fpu(void)
{
	int pid = sys_fork();
	if (pid < 0) FAIL("fork failed");
	if (pid == 0)
		child_exit(fpu_worker(1000.25, 20) == 0 ? 1 : 2);
	int mine = fpu_worker(7.5, 20);
	int theirs = sys_waitpid(pid);
	if (mine) FAIL("parent's x87 registers changed under it (round %d)", mine);
	if (theirs != 1) FAIL("child's x87 registers changed under it");

	/* A fresh image starts with a clean FPU, whatever its parent left. */
	__asm__ volatile ("fld1\n\tfld1\n\tfld1\n\tfld1\n\tfld1\n\tfld1\n\tfld1\n\tfld1");   /* fill the stack */
	pid = sys_fork();
	if (pid == 0) { sys_exec("ktchild"); child_exit(99); }
	__asm__ volatile ("fninit");
	int code = sys_waitpid(pid);
	if (code != 42) FAIL("exec'd child failed (%d)", code);
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

/* File data through every path that moves whole runs of sectors: big reads
 * and writes that start and end off a sector boundary, a write in the
 * middle, appends that grow a file in place and that move it, and a delete
 * that slides the files stored after it down the disk. */
#define FSR_SIZE 200000u
static unsigned char fsr_byte(unsigned int i, unsigned int salt)
{
	unsigned int v = (i + salt) * 2654435761u;
	return (unsigned char)(v >> 13);
}

static int fsr_check(const char *name, const unsigned char *want, unsigned int size,
                     unsigned char *scratch, const char *when)
{
	static const unsigned int offs[] = { 0, 1, 511, 512, 513, 65535, 65536, 70001, 131072 };
	static const unsigned int lens[] = { 1, 700, 4096, 66000, 131073 };
	int r = sys_fread(name, scratch, size + 4096);
	if (r != (int)size) FAIL("%s: %s reads %d bytes, want %u", when, name, r, size);
	if (memcmp(scratch, want, size) != 0) FAIL("%s: %s read back different", when, name);
	for (unsigned int a = 0; a < sizeof(offs) / sizeof(offs[0]); a++)
		for (unsigned int b = 0; b < sizeof(lens) / sizeof(lens[0]); b++) {
			unsigned int off = offs[a], len = lens[b];
			if (off >= size) continue;
			unsigned int n = len < size - off ? len : size - off;
			r = sys_fread_at(name, scratch, len, off);
			if (r != (int)n) FAIL("%s: read_at(%u, %u) returned %d", when, off, len, r);
			if (memcmp(scratch, want + off, n) != 0)
				FAIL("%s: read_at(%u, %u) returned other bytes", when, off, len);
		}
	return 0;
}

static int t_fs_runs(void)
{
	unsigned char *want = sys_mmap(FSR_SIZE + 2 * PAGE);
	unsigned char *scratch = sys_mmap(FSR_SIZE + 4 * PAGE);
	static unsigned char first[3000], third[100];
	if (!want || !scratch) FAIL("mmap failed");
	unsigned int size = FSR_SIZE;
	for (unsigned int i = 0; i < size; i++) want[i] = fsr_byte(i, 1);
	for (unsigned int i = 0; i < sizeof(first); i++) first[i] = fsr_byte(i, 7);
	for (unsigned int i = 0; i < sizeof(third); i++) third[i] = fsr_byte(i, 9);
	sys_fdelete("KTA.DAT"); sys_fdelete("KTB.DAT"); sys_fdelete("KTC.DAT");

	if (sys_fwrite("KTA.DAT", first, sizeof(first)) != 0) FAIL("could not create KTA.DAT");
	if (sys_fwrite("KTB.DAT", want, size) != 0) FAIL("could not create KTB.DAT");
	if (fsr_check("KTB.DAT", want, size, scratch, "after write")) return 1;

	/* overwrite a stretch in the middle, off the sector grid at both ends */
	for (unsigned int i = 0; i < 70000; i++) want[1000 + i] = fsr_byte(i, 3);
	if (sys_fwrite_at("KTB.DAT", want + 1000, 70000, 1000) != 0) FAIL("write_at failed");
	if (fsr_check("KTB.DAT", want, size, scratch, "after write_at")) return 1;

	/* last file on the disk: an append grows it where it is */
	for (unsigned int i = 0; i < 5000; i++) want[size + i] = fsr_byte(i, 4);
	if (sys_fappend("KTB.DAT", want + size, 5000) != 0) FAIL("append in place failed");
	size += 5000;
	if (fsr_check("KTB.DAT", want, size, scratch, "after append in place")) return 1;

	/* no longer the last file: the next append has to move it */
	if (sys_fwrite("KTC.DAT", third, sizeof(third)) != 0) FAIL("could not create KTC.DAT");
	for (unsigned int i = 0; i < 3000; i++) want[size + i] = fsr_byte(i, 5);
	if (sys_fappend("KTB.DAT", want + size, 3000) != 0) FAIL("append that moves failed");
	size += 3000;
	if (fsr_check("KTB.DAT", want, size, scratch, "after append that moved it")) return 1;

	/* deleting a file stored before them slides both down the disk */
	if (sys_fdelete("KTA.DAT") != 0) FAIL("delete failed");
	if (fsr_check("KTB.DAT", want, size, scratch, "after deleting an earlier file")) return 1;
	if (fsr_check("KTC.DAT", third, sizeof(third), scratch, "after deleting an earlier file")) return 1;

	sys_fdelete("KTB.DAT"); sys_fdelete("KTC.DAT");
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
	85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 98,
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

/* End to end through the network thread: DHCP has to bind and the
 * gateway has to answer an echo request. Receive processing runs in the
 * knet kernel thread, so this also proves a process blocked in a syscall
 * gets its replies. (QEMU user networking: gateway 10.0.2.2.) */
static int t_net_ping(void)
{
	struct sys_net_info ni;
	unsigned int t0 = sys_ticks();
	for (;;) {
		if (sys_netinfo(&ni) != 0) FAIL("netinfo failed");
		if (!ni.link_up) FAIL("no NIC");
		if (ni.dhcp_state == 3)
			break;                          /* bound */
		if ((unsigned int)(sys_ticks() - t0) > 800)
			FAIL("DHCP never bound (state %d)", ni.dhcp_state);
		sys_sleep_ms(50);
	}
	/* The first echo may be spent resolving the gateway's MAC. */
	int rtt = -1;
	for (int attempt = 0; attempt < 4 && rtt < 0; attempt++)
		rtt = sys_ping(sys_ip4(10, 0, 2, 2), 1000);
	if (rtt < 0) FAIL("gateway did not answer 4 pings");
	if (rtt > 500) FAIL("ping took %d ms", rtt);
	return 0;
}

/* The mixer runs in the sound card's interrupt, under whatever address
 * space happens to be loaded. It must not depend on the caller's buffer
 * staying mapped: unmap it mid-play, and play from a heap address that
 * does not exist in the other process sharing the CPU. */
static int t_audio_unmap(void)
{
	unsigned int base = free_pages();
	unsigned char *p = sys_mmap(4 * PAGE);
	if (!p) FAIL("mmap failed");
	for (unsigned int i = 0; i < 4 * PAGE; i++)
		p[i] = (unsigned char)(128 + ((i & 32) ? 20 : -20));
	int h = sys_audio_play(p, 4 * PAGE, 64);
	if (h < 0) FAIL("audio_play failed");
	if (sys_munmap(p, 4 * PAGE) != 0) FAIL("munmap failed");
	sys_sleep_ms(900);                  /* several sound IRQs with the buffer gone */

	/* Second half: a sound started here keeps playing while a child that
	 * has no mapping at this address runs. */
	unsigned char *q = sys_mmap(8 * PAGE);
	if (!q) FAIL("mmap failed");
	memset(q, 128, 8 * PAGE);
	int pid = sys_fork();
	if (pid < 0) FAIL("fork failed");
	if (pid == 0) { sys_exec("ktchild"); child_exit(99); }
	sys_audio_play(q, 8 * PAGE, 64);
	unsigned int t0 = sys_ticks();
	while ((unsigned int)(sys_ticks() - t0) < 60)
		;                               /* stay runnable so both share the CPU */
	sys_waitpid(pid);
	sys_munmap(q, 8 * PAGE);
	sys_sleep_ms(1700);                 /* let both sounds run out */
	unsigned int after = free_pages();
	if (after != base) FAIL("%d frames still held after the sounds ended", (int)(base - after));
	return 0;
}

/* How long a sound of known length takes in each output format. The
 * mixer consumes source samples at the output rate (22050 Hz), so 22050
 * samples last one second whatever the format; stereo used to finish in
 * half the time. Also checks handle hygiene: a stale handle must not
 * reach the sound that reused its channel. */
static int t_audio_modes(void)
{
	static unsigned char tone[22050];
	for (unsigned int i = 0; i < sizeof(tone); i++)
		tone[i] = (unsigned char)(128 + ((i & 16) ? 6 : -6));
	if (sys_audio_mode(99) != -1) FAIL("bogus mode accepted");
	if (sys_audio_query() < 0) FAIL("a rejected mode switch stopped audio output");

	for (int mode = 0; mode <= 2; mode++) {
		if (sys_audio_mode(mode) != 0) FAIL("mode %d refused", mode);
		sys_sleep_ms(450);              /* let the first buffers cycle */
		int h = sys_audio_play(tone, sizeof(tone), 32);
		if (h < 0) FAIL("play failed in mode %d", mode);
		unsigned int t0 = sys_ticks();
		while (sys_audio_playing(h) && (unsigned int)(sys_ticks() - t0) < 400)
			sys_sleep_ms(10);
		unsigned int took = sys_ticks() - t0;   /* 10 ms units; ideal = 100 */
		/* Draining is noticed one half-buffer (up to ~370 ms) late. */
		if (took < 80 || took > 160)
			FAIL("1.00 s of audio took %u0 ms in mode %d", took, mode);
		if (sys_audio_playing(h)) FAIL("ended sound still reported playing");

		int h2 = sys_audio_play(tone, sizeof(tone), 32);
		if (h2 == h) FAIL("handle reused for a new sound");
		sys_audio_stop(h);              /* stale: must not touch h2's sound */
		if (!sys_audio_playing(h2)) FAIL("stale handle stopped a newer sound (mode %d)", mode);
		sys_audio_stop(h2);
		if (sys_audio_playing(h2)) FAIL("stop did not stop");
	}
	sys_audio_mode(0);

	/* Another process cannot stop or mute our sound through its handle. */
	int h = sys_audio_play(tone, sizeof(tone), 32);
	int pid = sys_fork();
	if (pid < 0) FAIL("fork failed");
	if (pid == 0) {
		sys_audio_stop(h);
		sys_audio_set_volume(h, 0);
		child_exit(sys_audio_playing(h) ? 2 : 1);   /* not ours: reads as not playing */
	}
	int code = sys_waitpid(pid);
	if (code != 1) FAIL("child could see our channel (exit %d)", code);
	if (!sys_audio_playing(h)) FAIL("another process stopped our sound");
	sys_audio_stop(h);

	/* Longer than 65536 samples used to wrap the cursor and loop forever. */
	static unsigned char longtone[70000];
	memset(longtone, 128, sizeof(longtone));
	h = sys_audio_play(longtone, sizeof(longtone), 16);
	if (h < 0) FAIL("70000-sample sound refused");
	unsigned int t0 = sys_ticks();
	while (sys_audio_playing(h) && (unsigned int)(sys_ticks() - t0) < 600)
		sys_sleep_ms(20);
	if (sys_audio_playing(h)) FAIL("70000-sample sound never ended");
	return 0;
}

/* Pipes: an end belongs to the processes that were given it. */
static int t_pipes(void)
{
	/* 1. A process that was never handed the pipe cannot touch it by
	 *    guessing handle numbers. */
	int guesser = sys_fork();
	if (guesser < 0) FAIL("fork failed");
	if (guesser == 0) {
		sys_sleep_ms(150);              /* until the parent has made its pipe */
		int hits = 0;
		for (int h = 0; h < 32; h++) {
			if (sys_pipe_write(h, "x", 1) >= 0) hits++;
			if (sys_pipe_close(h) == 0) hits++;
		}
		child_exit(hits);
	}
	int fds[2];
	if (sys_pipe(fds) != 0) FAIL("pipe() failed");
	if (sys_pipe_write(fds[1], "ab", 2) != 2) FAIL("write to own pipe failed");
	int hits = sys_waitpid(guesser);
	if (hits != 0) FAIL("an unrelated process got %d operations through on our pipe", hits);
	char buf[8];
	if (sys_pipe_read(fds[0], buf, 2) != 2 || buf[0] != 'a' || buf[1] != 'b')
		FAIL("pipe contents were disturbed");

	/* 2. A child inherits both ends; when it dies holding the write end
	 *    and we close ours, the reader must see EOF instead of blocking. */
	int pid = sys_fork();
	if (pid < 0) FAIL("fork failed");
	if (pid == 0) {
		sys_pipe_write(fds[1], "hi", 2);
		child_exit(3);                  /* exits without closing anything */
	}
	if (sys_waitpid(pid) != 3) FAIL("child failed");
	sys_pipe_close(fds[1]);
	if (sys_pipe_read(fds[0], buf, 2) != 2 || buf[0] != 'h') FAIL("lost the child's bytes");
	int pid2 = sys_fork();
	if (pid2 < 0) FAIL("fork failed");
	if (pid2 == 0)
		child_exit(sys_pipe_read(fds[0], buf, 1) == 0 ? 1 : 2);   /* EOF, not a hang */
	/* waitpid would block forever on a stuck reader: give it time, then
	 * kill it (a no-op if it already exited cleanly with code 1). */
	sys_sleep_ms(300);
	sys_kill(pid2);                         /* no-op if it already exited cleanly */
	int eof = sys_waitpid(pid2);
	if (eof != 1) FAIL("reader did not get EOF after every writer was gone (exit %d)", eof);
	sys_pipe_close(fds[0]);

	/* 3. Processes that die with pipes open must not use the pool up. */
	for (int i = 0; i < 24; i++) {
		int c = sys_fork();
		if (c < 0) FAIL("fork failed");
		if (c == 0) {
			int f[2];
			child_exit(sys_pipe(f) == 0 ? 1 : 2);
		}
		if (sys_waitpid(c) != 1) FAIL("pipe pool exhausted after %d dead owners", i);
	}
	return 0;
}

/* SYS_MOUNT must not let a process alias the boot area or live data. */
static int t_mount_guard(void)
{
	if (sys_mount(1, 0, 0, 4, 200000) == 0)
		FAIL("mounted a volume over LBA 0 (boot sector + kernel)");
	if (sys_mount(1, 0, 512, 516, 200516) == 0)
		FAIL("mounted a second volume over the primary one");
	if (sys_mount(1, 0, 600, 604, 700) == 0)
		FAIL("mounted a volume inside the primary's used data");
	if (sys_mount(1, 0, 199000, 199004, 0x10000000u) == 0)
		FAIL("accepted a volume beyond the LBA28 limit");
	if (sys_mount(1, 0, 199000, 199001, 200000) == 0)
		FAIL("accepted a directory area shorter than the directory");
	/* The legitimate use still works: a volume in the free tail. */
	if (sys_mount(1, 0, 199000, 199004, 200000) != 0)
		FAIL("refused a volume in unused space");
	return 0;
}

/* Take memory until the kernel says no, use every byte of it, give it all
 * back. Running out must be an orderly "no", every page handed out must be
 * real memory that holds what was written to it, and nothing may be lost.
 * Run with less RAM than the allocator's compile-time limit (--mem 24)
 * this also checks that the kernel sized itself to the machine. */
static int t_oom(void)
{
	enum { CHUNK = 256 * 1024, MAXC = 56 };     /* heap region is ~14.7 MiB */
	static unsigned char *chunk[MAXC];
	unsigned int base = free_pages();
	int n = 0;
	while (n < MAXC) {
		unsigned char *p = sys_mmap(CHUNK);
		if (!p)
			break;
		for (unsigned int off = 0; off < CHUNK; off += 1024)
			p[off] = (unsigned char)(n * 31 + (off >> 10));
		chunk[n++] = p;
	}
	if (n == 0) FAIL("could not map even 256 KiB (free=%u pages)", base);
	for (int i = 0; i < n; i++)
		for (unsigned int off = 0; off < CHUNK; off += 1024)
			if (chunk[i][off] != (unsigned char)(i * 31 + (off >> 10)))
				FAIL("chunk %d of %d lost its contents at offset %u", i, n, off);
	/* While we hold ours, a second process does the same. Together the
	 * two ask for more than a small machine has: the second must be told
	 * no at some point, and until then every page it gets must be real. */
	int pid = sys_fork();
	if (pid == 0) { sys_exec("ktmem"); child_exit(99); }
	if (pid > 0) {
		int code = sys_waitpid(pid);
		if (code == 43) FAIL("a second process was given memory that does not hold data");
		if (code != 42 && code != 44) FAIL("second process ended with %d", code);
	}
	for (int i = 0; i < n; i++)
		for (unsigned int off = 0; off < CHUNK; off += 1024)
			if (chunk[i][off] != (unsigned char)(i * 31 + (off >> 10)))
				FAIL("our chunk %d was overwritten while another process ran", i);
	for (int i = 0; i < n; i++)
		if (sys_munmap(chunk[i], CHUNK) != 0) FAIL("munmap failed");
	unsigned int after = free_pages();
	if (after != base) FAIL("lost %d frames after mapping %d chunks", (int)(base - after), n);
	sys_printf("ktest: oom: mapped %d KiB before the limit, %u pages free at start",
	           n * (CHUNK / 1024), base);
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

/* ---- windows ---------------------------------------------------------------
 *
 * The window manager from the process side: its buffers are real memory
 * that must be handed back, its mappings must go away with the window, and
 * a hostile or clumsy caller must get -1, not a broken kernel. Pointer and
 * keyboard behaviour is covered from outside by tools/qa/wm_test.py.
 */

#define WINPX ((volatile unsigned int *)USER_WIN_BASE)

/* Create, show, draw, flush; returns 0 or the failing step. */
static int win_open(unsigned int w, unsigned int h)
{
	if (sys_win_create(w, h, 0) != 0)
		return 1;
	if (sys_win_show(-1, -1, "ktest") != 0)
		return 2;
	for (unsigned int i = 0; i < w * h; i += 97)
		WINPX[i] = 0x00336699u;
	WINPX[w * h - 1] = 0x00FFFFFFu;         /* the very last pixel is mapped too */
	sys_win_flush();
	return 0;
}

static int t_win_leak(void)
{
	for (int warm = 0; warm < 2; warm++) {
		int pid = sys_fork();
		if (pid < 0) FAIL("warm-up fork failed");
		if (pid == 0) child_exit(win_open(320, 200) == 0 ? 1 : 2);
		sys_waitpid(pid);
	}
	unsigned int base = free_pages();
	for (int i = 0; i < 16; i++) {
		int pid = sys_fork();
		if (pid < 0) FAIL("fork %d failed", i);
		if (pid == 0) {
			if (win_open(320, 200) != 0)
				child_exit(2);
			if (i % 4 == 1 && sys_win_resize(500, 300) != 0)
				child_exit(3);
			if (i % 4 == 2 && sys_win_resize(130, 70) != 0)
				child_exit(4);
			sys_sleep_ms(20);
			if (i % 2)
				sys_win_destroy();          /* half leave it to the kernel */
			child_exit(1);
		}
		int code = sys_waitpid(pid);
		if (code != 1) FAIL("child %d failed at step %d", i, code);
	}
	unsigned int after = free_pages();
	if (after != base) FAIL("leaked %d frames over 16 window lifetimes", (int)(base - after));
	return 0;
}

static int t_win_limits(void)
{
	if (sys_win_create(0, 100, 0) == 0)            FAIL("accepted width 0");
	if (sys_win_create(100, 0, 0) == 0)            FAIL("accepted height 0");
	if (sys_win_create(8, 8, 0) == 0)              FAIL("accepted a window smaller than its chrome");
	if (sys_win_create(4000, 100, 0) == 0)         FAIL("accepted a window wider than the screen");
	if (sys_win_create(100, 4000, 0) == 0)         FAIL("accepted a window taller than the screen");
	if (sys_win_create(0x10000u, 0x10000u, 0) == 0) FAIL("accepted w*h that overflows 32 bits");
	if (sys_win_create(0xFFFFFFFFu, 2, 0) == 0)    FAIL("accepted a huge width");
	if (sys_win_create(300, 200, 0xF0u) == 0)      FAIL("accepted unknown flags");
	if (sys_win_show(10, 10, "none") == 0)         FAIL("showed a window that does not exist");
	if (sys_win_resize(300, 200) == 0)             FAIL("resized a window that does not exist");
	struct stink_win_event ev;
	if (sys_win_get_event(&ev) == 0)               FAIL("got an event without a window");
	if (sys_win_wait(10) != -1)                    FAIL("waited on a window that does not exist");
	sys_win_destroy();                              /* no window: must be a no-op */

	if (win_open(300, 200) != 0)                   FAIL("could not open a plain window");
	if (sys_win_create(300, 200, 0) == 0)          FAIL("one process got two windows");
	if (sys_win_resize(0, 0) == 0)                 FAIL("resize to 0x0 accepted");
	if (sys_win_resize(5000, 5000) == 0)           FAIL("resize beyond the screen accepted");
	WINPX[300 * 200 - 1] = 1;                       /* failed resizes left the buffer alone */
	if (sys_win_resize(640, 400) != 0)             FAIL("valid resize refused");
	WINPX[640 * 400 - 1] = 1;                       /* new buffer fully mapped */
	if (sys_win_get_event((struct stink_win_event *)0x80100000u) == 0)
		FAIL("event written through a kernel pointer");
	if (sys_win_show(-5000, -5000, (const char *)0xFFFFF000u) != 0)
		FAIL("an unreadable title pointer broke show");
	/* A window owner draws into its buffer, not on the screen. */
	if (__syscall(22, 0, (10 << 16) | 10, 0xFFFFFF) != -1)
		FAIL("a window owner drew straight to the framebuffer");
	if (sys_getkey() != 0)                          FAIL("unfocused/idle window read a key");
	sys_win_destroy();
	if (win_open(200, 100) != 0)                   FAIL("slot not reusable after destroy");
	sys_win_destroy();

	/* More processes than window slots: the extra ones are told no. */
	int pids[10], opened = 0;
	for (int i = 0; i < 10; i++) {
		pids[i] = sys_fork();
		if (pids[i] < 0) FAIL("fork failed");
		if (pids[i] == 0) {
			int ok = (win_open(160, 90) == 0);
			sys_sleep_ms(400);
			child_exit(ok ? 1 : 2);
		}
	}
	for (int i = 0; i < 10; i++)
		if (sys_waitpid(pids[i]) == 1)
			opened++;
	if (opened != 8) FAIL("%d of 10 processes got a window, the limit is 8", opened);
	if (win_open(200, 100) != 0) FAIL("slots not released when their owners exited");
	sys_win_destroy();
	return 0;
}

/* The buffer mapping dies with the window (and shrinks with it). */
static int t_win_unmap(void)
{
	int pid = sys_fork();
	if (pid < 0) FAIL("fork failed");
	if (pid == 0) {
		if (win_open(300, 200) != 0) child_exit(2);
		sys_win_destroy();
		WINPX[0] = 1;                       /* must fault */
		child_exit(3);
	}
	int code = sys_waitpid(pid);
	if (code != 139) FAIL("write to a destroyed window's buffer did not fault (exit %d)", code);

	pid = sys_fork();
	if (pid < 0) FAIL("fork failed");
	if (pid == 0) {
		if (win_open(600, 400) != 0) child_exit(2);
		if (sys_win_resize(130, 70) != 0) child_exit(4);
		WINPX[130 * 70 - 1] = 1;            /* still ours */
		WINPX[600 * 400 - 1] = 1;           /* gone with the old buffer: must fault */
		child_exit(3);
	}
	code = sys_waitpid(pid);
	if (code != 139) FAIL("old buffer still mapped after shrinking (exit %d)", code);

	/* A window belongs to one process. A child forked while it is open
	 * gets neither the window nor a view of its pixels. */
	pid = sys_fork();
	if (pid < 0) FAIL("fork failed");
	if (pid == 0) {
		if (win_open(200, 100) != 0) child_exit(2);
		WINPX[0] = 0x123456;
		int kid = sys_fork();
		if (kid < 0) child_exit(4);
		if (kid == 0) {
			struct stink_win_event ev;
			if (sys_win_get_event(&ev) != -1) child_exit(5);   /* no window here */
			WINPX[0] = 1;                   /* must fault */
			child_exit(6);
		}
		int kc = sys_waitpid(kid);
		if (kc != 139) child_exit(kc == 5 ? 5 : 7);
		if (WINPX[0] != 0x123456) child_exit(8);              /* ours, untouched */
		child_exit(1);
	}
	code = sys_waitpid(pid);
	if (code == 5) FAIL("a forked child could use its parent's window");
	if (code == 7) FAIL("a forked child could write its parent's window buffer");
	if (code == 8) FAIL("the parent's window buffer changed across a fork");
	if (code != 1) FAIL("fork with a window open: exit %d", code);
	return 0;
}

/* exec and kill both take the window with them. */
static int t_win_exec_kill(void)
{
	for (int warm = 0; warm < 2; warm++) {
		int pid = sys_fork();
		if (pid < 0) FAIL("warm-up fork failed");
		if (pid == 0) { win_open(320, 200); sys_exec("ktchild"); child_exit(99); }
		sys_waitpid(pid);
	}
	/* Our own first window allocates the page table for the buffer range,
	 * which stays with the address space: do that before the baseline. */
	if (win_open(200, 100) != 0) FAIL("could not open a window");
	sys_win_destroy();
	unsigned int base = free_pages();
	for (int i = 0; i < 6; i++) {
		int pid = sys_fork();
		if (pid < 0) FAIL("fork failed");
		if (pid == 0) {
			if (win_open(320, 200) != 0) child_exit(2);
			sys_exec("ktchild");            /* new image: the window must not survive */
			child_exit(99);
		}
		int code = sys_waitpid(pid);
		if (code != 42) FAIL("exec'd child exit code %d, want 42", code);
	}
	for (int i = 0; i < 6; i++) {
		int pid = sys_fork();
		if (pid < 0) FAIL("fork failed");
		if (pid == 0) {
			if (win_open(320, 200) != 0) child_exit(2);
			for (;;)
				sys_win_wait(1000);
		}
		sys_sleep_ms(40);
		if (sys_kill(pid) != 0) FAIL("kill failed");
		int code = sys_waitpid(pid);
		if (code != 137) FAIL("killed window owner exit code %d, want 137", code);
	}
	if (win_open(200, 100) != 0) FAIL("window slots leaked by exec or kill");
	sys_win_destroy();
	unsigned int after = free_pages();
	if (after != base) FAIL("leaked %d frames over exec/kill of window owners", (int)(base - after));
	return 0;
}

/* Not a check by itself: sits for a while without reading any input and
 * then returns to whatever started it. tools/qa/input_flush.py types keys
 * during the pause and verifies they are not replayed into the menu. */
static int t_idle_exit(void)
{
	sys_sleep_ms(2500);
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
	{ "procinfo",    t_procinfo },
	{ "sleep-fair",  t_sleep_fair },
	{ "preempt",     t_preempt },
	{ "resched",     t_resched },
	{ "fair3",       t_fair3 },
	{ "fpu",         t_fpu },
	{ "concurrent",  t_concurrent },
	{ "fs-runs",     t_fs_runs },
	{ "sbrk-oom",    t_sbrk_oom },
	{ "oom",         t_oom },
	{ "eflags",      t_eflags },
	{ "net-ping",    t_net_ping },
	{ "audio-unmap", t_audio_unmap },
	{ "audio-modes", t_audio_modes },
	{ "pipes",       t_pipes },
	{ "mount-guard", t_mount_guard },
	{ "win-leak",    t_win_leak },
	{ "win-limits",  t_win_limits },
	{ "win-unmap",   t_win_unmap },
	{ "win-exec-kill", t_win_exec_kill },
	{ "badptr",      t_badptr },
	{ "idle-exit",   t_idle_exit },       /* driven by input_flush.py, not by "all" */
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
	int stay = (strcmp(arg, "idle-exit") == 0);   /* return to the menu instead of powering off */

	int ran = 0;
	for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		if (all ? cases[i].fn == t_idle_exit : strcmp(arg, cases[i].name) != 0)
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
	if (!stay)
		sys_shutdown();
}
