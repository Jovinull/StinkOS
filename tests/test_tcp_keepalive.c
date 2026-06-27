/* Host-side test for the TCP keepalive timer in
 * kernel/drivers/net/tcp.c. Once an ESTABLISHED connection has been
 * idle for TCP_KEEPALIVE_IDLE ticks, the stack starts sending probe
 * ACKs every TCP_KEEPALIVE_INTERVAL ticks. Any RX activity resets
 * the idle counter, suppressing further probes.
 *
 * We mirror the decision logic (probe-or-not) for a single TCB.
 * The actual probe wire encoding (seq = snd_una - 1, ACK flag only,
 * no payload) is covered by the live network and the inline comment
 * in the kernel; here the contract is the timer.
 */
#include <stdio.h>

#define TCP_KEEPALIVE_IDLE     7200u
#define TCP_KEEPALIVE_INTERVAL 3000u

struct tcb_sim {
	unsigned int last_activity_ticks;
	unsigned int last_keepalive_ticks;
};

/* Returns 1 if the kernel would emit a keepalive probe AT THIS TICK
 * for the given TCB. Mirrors the gate at tcp.c:1201-1212. */
static int should_probe(const struct tcb_sim *t, unsigned int now)
{
	if (t->last_activity_ticks == 0) return 0;
	unsigned int idle = now - t->last_activity_ticks;
	if (idle < TCP_KEEPALIVE_IDLE) return 0;
	unsigned int since_probe = (t->last_keepalive_ticks == 0)
	                            ? TCP_KEEPALIVE_INTERVAL
	                            : (now - t->last_keepalive_ticks);
	return since_probe >= TCP_KEEPALIVE_INTERVAL;
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
	struct tcb_sim t = { 0, 0 };

	/* Fresh slot with no traffic ever: last_activity == 0 -> never
	 * probes (the connection isn't really established yet). */
	failures += expect_int("never-active TCB -> no probe",
	                       should_probe(&t, 100000u), 0);

	/* Active just now: 0 ticks idle -> no probe. */
	t.last_activity_ticks  = 1000;
	t.last_keepalive_ticks = 0;
	failures += expect_int("idle = 0 -> no probe",
	                       should_probe(&t, 1000u), 0);

	/* Idle = IDLE - 1: still below threshold, no probe. */
	failures += expect_int("idle = IDLE-1 -> no probe",
	                       should_probe(&t, 1000u + TCP_KEEPALIVE_IDLE - 1u), 0);

	/* Idle = IDLE exactly, no prior probe: first probe fires.
	 * (since_probe defaults to TCP_KEEPALIVE_INTERVAL, which clears
	 * the >= gate.) */
	failures += expect_int("idle = IDLE, first probe -> 1",
	                       should_probe(&t, 1000u + TCP_KEEPALIVE_IDLE), 1);

	/* After probe at t=8200: another probe needs INTERVAL more ticks. */
	t.last_keepalive_ticks = 1000u + TCP_KEEPALIVE_IDLE;
	failures += expect_int("post-probe immediate -> no",
	                       should_probe(&t, 1000u + TCP_KEEPALIVE_IDLE), 0);
	failures += expect_int("post-probe + INTERVAL-1 -> no",
	                       should_probe(&t,
	                                    1000u + TCP_KEEPALIVE_IDLE +
	                                    TCP_KEEPALIVE_INTERVAL - 1u), 0);
	failures += expect_int("post-probe + INTERVAL -> probe",
	                       should_probe(&t,
	                                    1000u + TCP_KEEPALIVE_IDLE +
	                                    TCP_KEEPALIVE_INTERVAL), 1);

	/* RX activity resets last_activity -> the idle gate falls back
	 * below IDLE and probes stop. Mirror that here. */
	t.last_activity_ticks  = 50000u;
	t.last_keepalive_ticks = 49000u;
	failures += expect_int("post-RX, idle = 0 -> no probe",
	                       should_probe(&t, 50000u), 0);
	failures += expect_int("post-RX, idle = IDLE-1 -> no probe",
	                       should_probe(&t,
	                                    50000u + TCP_KEEPALIVE_IDLE - 1u), 0);

	/* Tick counter wraparound: idle = now - last_activity uses unsigned
	 * subtraction, so a fresh activity stamp right before rollover
	 * is still measured correctly. */
	t.last_activity_ticks  = 0xFFFFFFFFu - 100u;
	t.last_keepalive_ticks = 0;
	failures += expect_int("near rollover, idle < IDLE -> no probe",
	                       should_probe(&t, 0xFFFFFFFFu), 0);
	failures += expect_int("post-rollover, idle = IDLE -> probe",
	                       should_probe(&t,
	                                    (0xFFFFFFFFu - 100u) + TCP_KEEPALIVE_IDLE),
	                       1);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
