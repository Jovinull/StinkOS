/* Host-side test for the TCP TIME_WAIT 2*MSL cleanup in
 * kernel/drivers/net/tcp.c. Without this gate, the 8-entry TCB table
 * leaks one slot per graceful close until reboot.
 *
 * Policy:
 *   - On entering TIME_WAIT, stamp time_wait_ticks = pit_ticks().
 *   - tcp_tick: if state == TIME_WAIT and (now - time_wait_ticks)
 *     >= TCP_2MSL_TICKS, transition to CLOSED and free in_use.
 *   - Any state other than TIME_WAIT must NOT be touched by the gate.
 *
 * TCP_2MSL_TICKS = 6000 (60 s at 100 Hz PIT).
 */
#include <stdio.h>

#define TCP_2MSL_TICKS 6000u

enum { CLOSED = 0, ESTABLISHED = 4, TIME_WAIT = 10 };

struct tcb {
	int          state;
	unsigned int time_wait_ticks;
	int          in_use;
};

/* Replica of the tick gate. Returns 1 if the slot was freed this call. */
static int tick_timewait(struct tcb *t, unsigned int now)
{
	if (t->state == TIME_WAIT &&
	    (now - t->time_wait_ticks) >= TCP_2MSL_TICKS) {
		t->state  = CLOSED;
		t->in_use = 0;
		return 1;
	}
	return 0;
}

static int expect_int(const char *label, int got, int want)
{
	if (got == want) { printf("ok   %-50s = %d\n", label, got); return 0; }
	printf("FAIL %s: got %d, want %d\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;
	struct tcb t;

	/* Just-entered TIME_WAIT: cleanup must NOT fire. */
	t.state = TIME_WAIT; t.time_wait_ticks = 1000; t.in_use = 1;
	failures += expect_int("entry t+0: not freed",     tick_timewait(&t, 1000),  0);
	failures += expect_int("entry t+0: state kept",    t.state,                  TIME_WAIT);
	failures += expect_int("entry t+0: in_use kept",   t.in_use,                 1);

	/* Mid-window: still pinned. */
	failures += expect_int("t+3000: not freed",        tick_timewait(&t, 4000),  0);
	failures += expect_int("t+5999: still not freed",  tick_timewait(&t, 6999),  0);

	/* Boundary: exactly 6000 ticks later -> free. */
	t.state = TIME_WAIT; t.time_wait_ticks = 1000; t.in_use = 1;
	failures += expect_int("t+6000: freed",            tick_timewait(&t, 7000),  1);
	failures += expect_int("t+6000: state = CLOSED",   t.state,                  CLOSED);
	failures += expect_int("t+6000: in_use cleared",   t.in_use,                 0);

	/* Long-overdue tick (well past 2*MSL) also frees. */
	t.state = TIME_WAIT; t.time_wait_ticks = 100; t.in_use = 1;
	failures += expect_int("t+1e6: freed",             tick_timewait(&t, 1000000), 1);

	/* Non-TIME_WAIT state must be untouched by the gate even past 2*MSL. */
	t.state = ESTABLISHED; t.time_wait_ticks = 100; t.in_use = 1;
	failures += expect_int("ESTABLISHED ignored",      tick_timewait(&t, 1000000), 0);
	failures += expect_int("ESTABLISHED state kept",   t.state,                    ESTABLISHED);
	failures += expect_int("ESTABLISHED in_use kept",  t.in_use,                   1);

	/* CLOSED state ignored (already free). */
	t.state = CLOSED; t.time_wait_ticks = 0; t.in_use = 0;
	failures += expect_int("CLOSED ignored",           tick_timewait(&t, 1000000), 0);

	/* Counter wraparound: unsigned subtraction stays correct. */
	t.state = TIME_WAIT; t.time_wait_ticks = 0xFFFFFFFFu - 10u; t.in_use = 1;
	failures += expect_int("wrap: t before window not freed",
	                       tick_timewait(&t, 0xFFFFFFFFu + 1000u), 0);
	t.state = TIME_WAIT; t.time_wait_ticks = 0xFFFFFFFFu - 10u; t.in_use = 1;
	failures += expect_int("wrap: t past window freed",
	                       tick_timewait(&t, 0xFFFFFFFFu + 5990u), 1);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
