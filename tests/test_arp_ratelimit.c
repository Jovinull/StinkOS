/* Host-side test for the ARP request rate limiter added to
 * kernel/drivers/net/arp.c (arp_send_request). Without it, ipv4_send
 * would fire arp_send_request on every TX that hit an unresolved peer,
 * flooding the LAN with ARP broadcasts.
 *
 * Policy: at most one request every 10 PIT ticks (= 100 ms at 100 Hz).
 * First call is always allowed (last_request_tick == 0 sentinel).
 */
#include <stdio.h>

#define ARP_RATELIMIT_TICKS 10u

static unsigned int last_request_tick;

static void reset(void) { last_request_tick = 0; }

/* Returns 1 if the kernel would emit the ARP request, 0 if rate-limited. */
static int try_send(unsigned int now)
{
	if (last_request_tick != 0 &&
	    (now - last_request_tick) < ARP_RATELIMIT_TICKS)
		return 0;
	last_request_tick = now;
	return 1;
}

static int expect_int(const char *label, int got, int want)
{
	if (got == want) { printf("ok   %-45s = %d\n", label, got); return 0; }
	printf("FAIL %s: got %d, want %d\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;

	reset();
	failures += expect_int("first call sends",          try_send(100),     1);
	failures += expect_int("t+1 dropped",               try_send(101),     0);
	failures += expect_int("t+9 dropped",               try_send(109),     0);
	failures += expect_int("t+10 sends (boundary)",     try_send(110),     1);
	failures += expect_int("after boundary: t+11 drop", try_send(111),     0);

	/* 1000 calls at same tick: only 1 send. */
	reset();
	int sends = 0;
	for (int i = 0; i < 1000; i++) sends += try_send(50);
	failures += expect_int("1000 calls at same tick: 1 send", sends, 1);

	/* 100 calls at t=0..99 (every tick): first allowed (t=0), then
	 * one every 10 ticks (t=10, 20, ..., 90) -- total 11 sends. */
	reset();
	sends = 0;
	for (unsigned int t = 0; t < 100; t++) sends += try_send(t);
	failures += expect_int("100 calls across 1s: 11 sends", sends, 11);

	/* Counter wraparound: (now - last) unsigned-subtracts correctly. */
	reset();
	try_send(0xFFFFFFFFu - 3u);                 /* near rollover */
	failures += expect_int("post-rollover t+2 dropped",
	                        try_send(0xFFFFFFFFu - 1u),         0);
	failures += expect_int("post-rollover t+20 sends",
	                        try_send(0xFFFFFFFFu + 17u),        1);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
