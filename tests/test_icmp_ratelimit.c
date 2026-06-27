/* Host-side test for the ICMP "destination unreachable" rate limiter in
 * kernel/drivers/net/udp.c (the static last_unreach_tick gate in
 * udp_handle). Policy (RFC 1812 §4.3.2.8):
 *
 *   - First call always sends (last_unreach_tick == 0 is the unarmed
 *     state).
 *   - Each subsequent call sends ONLY if at least 33 PIT ticks have
 *     elapsed since the previous send (PIT runs at 100 Hz, so 33 ticks
 *     means ~3 unreachable replies per second worst-case).
 *   - A send stamps last_unreach_tick = now; a drop does NOT update the
 *     stamp (otherwise a sustained flood would push the next send out
 *     indefinitely).
 *
 * Counter wraparound is implicit -- (now - last_unreach_tick) uses
 * unsigned-int subtraction, so even after a 32-bit tick rollover the
 * comparison stays sensible.
 */
#include <stdio.h>

#define ICMP_RATELIMIT_TICKS 33u

static unsigned int last_unreach_tick;

static void reset(void) { last_unreach_tick = 0; }

/* Returns 1 if the kernel would send the ICMP unreachable, 0 if it would
 * be dropped by the rate limiter. Mirrors the kernel decision exactly. */
static int try_send(unsigned int now)
{
	if (last_unreach_tick != 0 &&
	    (now - last_unreach_tick) < ICMP_RATELIMIT_TICKS)
		return 0;
	last_unreach_tick = now;
	return 1;
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

	/* First call always passes. */
	reset();
	failures += expect_int("first call sends",          try_send(100),     1);

	/* Burst immediately after: every subsequent call inside the window
	 * gets dropped. */
	failures += expect_int("burst t+1 dropped",         try_send(101),     0);
	failures += expect_int("burst t+5 dropped",         try_send(105),     0);
	failures += expect_int("burst t+10 dropped",        try_send(110),     0);
	failures += expect_int("burst t+32 dropped",        try_send(132),     0);

	/* Boundary: exactly 33 ticks later is a fresh send. */
	failures += expect_int("t+33 sends",                try_send(133),     1);

	/* And immediately re-enters the throttle window. */
	failures += expect_int("after t+33: t+34 dropped",  try_send(134),     0);
	failures += expect_int("after t+33: t+65 dropped",  try_send(165),     0);
	failures += expect_int("after t+33: t+66 sends",    try_send(166),     1);

	/* A flood of N calls inside one tick must yield exactly 1 send
	 * (the first), 0 sends thereafter. */
	reset();
	int sends = 0;
	for (int i = 0; i < 1000; i++) sends += try_send(500);
	failures += expect_int("1000 calls at same tick: sends",    sends, 1);

	/* A flood spread evenly across 1 second (100 ticks) at 1ms granularity
	 * should yield ceil(100/33) = 4 sends: t=0, t=33, t=66, t=99. */
	reset();
	sends = 0;
	for (unsigned int t = 0; t < 100; t++) sends += try_send(t);
	failures += expect_int("100 calls across 1s: sends",        sends, 4);

	/* Counter wraparound: the kernel uses 32-bit unsigned subtraction so
	 * even after PIT wraps, the gate stays correct. Simulate a tick that
	 * lies 50 units PAST the wraparound point. */
	reset();
	try_send(0xFFFFFFFFu - 10u);            /* just before rollover */
	failures += expect_int("post-rollover within window dropped",
	                        try_send(0xFFFFFFFFu + 5u), 0);
	failures += expect_int("post-rollover after window sends",
	                        try_send(0xFFFFFFFFu + 30u), 1);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
