/* Host-side test for the per-PID TCB reaper added to
 * kernel/drivers/net/tcp.c (tcp_close_pid). Process teardown calls
 * this from SYS_EXIT and SYS_KILL so a dying app cannot pin the
 * 8-slot connection table forever.
 *
 * Policy:
 *   - pid <= 0 is a no-op (kernel-internal owner -- defensive).
 *   - For every in_use TCB whose owner_pid matches `pid`, drive
 *     tcp_close (which either FINs or drops to CLOSED).
 *   - TCBs owned by OTHER pids stay untouched.
 *   - TCBs not in_use stay untouched.
 */
#include <stdio.h>

#define MAX_CONNS 8

enum { CLOSED, LISTEN, SYN_SENT, ESTABLISHED, FIN_WAIT_1, LAST_ACK };

struct tcb {
	int in_use;
	int state;
	int owner_pid;
};

static struct tcb conns[MAX_CONNS];

/* Mirror of tcp_close: ESTABLISHED -> FIN_WAIT_1 (don't free yet);
 * anything else (LISTEN, SYN_SENT, etc.) -> CLOSED + in_use = 0. */
static void close_one(struct tcb *t)
{
	switch (t->state) {
	case ESTABLISHED:
		t->state = FIN_WAIT_1;
		break;
	default:
		t->state  = CLOSED;
		t->in_use = 0;
		break;
	}
}

/* Mirror of tcp_close_pid. */
static void close_pid(int pid)
{
	if (pid <= 0) return;
	for (int i = 0; i < MAX_CONNS; i++) {
		if (!conns[i].in_use)         continue;
		if (conns[i].owner_pid != pid) continue;
		close_one(&conns[i]);
	}
}

static int expect_int(const char *label, int got, int want)
{
	if (got == want) { printf("ok   %-50s = %d\n", label, got); return 0; }
	printf("FAIL %s: got %d, want %d\n", label, got, want);
	return 1;
}

static void seed(int idx, int state, int pid)
{
	conns[idx].in_use    = 1;
	conns[idx].state     = state;
	conns[idx].owner_pid = pid;
}

int main(void)
{
	int failures = 0;

	/* Seed: 4 TCBs owned by pid=5, 3 by pid=7, 1 free slot. */
	for (int i = 0; i < MAX_CONNS; i++) conns[i] = (struct tcb){0};
	seed(0, ESTABLISHED, 5);
	seed(1, LISTEN,      5);
	seed(2, ESTABLISHED, 7);
	seed(3, SYN_SENT,    5);
	seed(4, ESTABLISHED, 7);
	seed(5, LISTEN,      7);
	seed(6, ESTABLISHED, 5);
	/* slot 7 stays free */

	/* Defensive: pid 0 is no-op. */
	close_pid(0);
	int alive = 0;
	for (int i = 0; i < MAX_CONNS; i++) alive += conns[i].in_use;
	failures += expect_int("pid 0: nothing touched (7 alive)", alive, 7);

	close_pid(-1);
	alive = 0;
	for (int i = 0; i < MAX_CONNS; i++) alive += conns[i].in_use;
	failures += expect_int("pid -1: nothing touched",          alive, 7);

	/* Close pid 5. Owned slots: 0, 1, 3, 6.
	 *   slot 0 ESTABLISHED -> FIN_WAIT_1 (in_use stays 1)
	 *   slot 1 LISTEN      -> CLOSED + in_use=0
	 *   slot 3 SYN_SENT    -> CLOSED + in_use=0
	 *   slot 6 ESTABLISHED -> FIN_WAIT_1 (in_use stays 1)
	 * pid 7 slots untouched. */
	close_pid(5);
	failures += expect_int("slot 0 (pid 5 EST): FIN_WAIT_1",   conns[0].state,   FIN_WAIT_1);
	failures += expect_int("slot 0: still in_use",             conns[0].in_use,  1);
	failures += expect_int("slot 1 (pid 5 LISTEN): freed",     conns[1].in_use,  0);
	failures += expect_int("slot 3 (pid 5 SYN_SENT): freed",   conns[3].in_use,  0);
	failures += expect_int("slot 6 (pid 5 EST): FIN_WAIT_1",   conns[6].state,   FIN_WAIT_1);

	/* pid 7 untouched. */
	failures += expect_int("slot 2 (pid 7): state kept",       conns[2].state,   ESTABLISHED);
	failures += expect_int("slot 4 (pid 7): state kept",       conns[4].state,   ESTABLISHED);
	failures += expect_int("slot 5 (pid 7): state kept",       conns[5].state,   LISTEN);
	failures += expect_int("slot 2 owner kept = 7",            conns[2].owner_pid, 7);

	/* Close pid 7 too. */
	close_pid(7);
	failures += expect_int("slot 2 (pid 7 EST): FIN_WAIT_1",   conns[2].state,   FIN_WAIT_1);
	failures += expect_int("slot 4 (pid 7 EST): FIN_WAIT_1",   conns[4].state,   FIN_WAIT_1);
	failures += expect_int("slot 5 (pid 7 LISTEN): freed",     conns[5].in_use,  0);

	/* Close pid that owns nothing: no-op. */
	close_pid(99);
	int unchanged = 1;
	if (conns[0].state != FIN_WAIT_1) unchanged = 0;
	failures += expect_int("close unknown pid: no-op",         unchanged, 1);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
