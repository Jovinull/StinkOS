/* Host-side test for the DNS retry pump in kernel/drivers/net/dns.c
 * (the dns_tick function added alongside the existing single-shot
 * resolver). Policy:
 *
 *   - dns_resolve() sends once and stamps last_send_tick = now,
 *     send_retries = 0.
 *   - dns_tick is a no-op when answer_ready (reply landed) OR when
 *     last_send_tick == 0 (no query in flight).
 *   - Otherwise, if send_retries >= DNS_MAX_RETRIES, set last_send_tick
 *     to 0 (give up; the slot is now inert).
 *   - Otherwise, if (now - last_send_tick) >= DNS_RETRY_TICKS,
 *     retransmit, refresh last_send_tick = now, and bump send_retries.
 *
 * Defaults: DNS_RETRY_TICKS = 200 (2 s), DNS_MAX_RETRIES = 3 -- worst-
 * case latency ~8 s before the caller's app-level timeout would notice.
 */
#include <stdio.h>

#define DNS_RETRY_TICKS 200u
#define DNS_MAX_RETRIES 3u

struct sim {
	int           answer_ready;
	unsigned int  last_send_tick;
	unsigned int  send_retries;
	unsigned int  retransmits;     /* count of wire sends this run */
};

static void sim_start(struct sim *s, unsigned int now)
{
	s->answer_ready   = 0;
	s->last_send_tick = now;
	s->send_retries   = 0;
	s->retransmits    = 0;
}

static void sim_tick(struct sim *s, unsigned int now)
{
	if (s->answer_ready || s->last_send_tick == 0)
		return;
	if (s->send_retries >= DNS_MAX_RETRIES) {
		s->last_send_tick = 0;
		return;
	}
	if ((now - s->last_send_tick) < DNS_RETRY_TICKS)
		return;
	s->retransmits++;
	s->last_send_tick = now;
	s->send_retries++;
}

static void sim_on_reply(struct sim *s)
{
	s->answer_ready   = 1;
	s->last_send_tick = 0;
}

static int expect_uint(const char *label, unsigned int got, unsigned int want)
{
	if (got == want) { printf("ok   %-50s = %u\n", label, got); return 0; }
	printf("FAIL %s: got %u, want %u\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;
	struct sim s;

	/* No query in flight: tick is a no-op forever. */
	s = (struct sim){0};
	for (unsigned int i = 0; i < 50; i++) sim_tick(&s, i * 100);
	failures += expect_uint("idle: no retransmits",      s.retransmits, 0);

	/* Single query, reply lands within window: no retransmits. */
	sim_start(&s, 1000);
	for (unsigned int t = 1000; t < 1150; t += 10) sim_tick(&s, t);
	sim_on_reply(&s);
	for (unsigned int t = 1150; t < 2000; t += 10) sim_tick(&s, t);
	failures += expect_uint("reply within window: 0 retransmits",
	                        s.retransmits, 0);
	failures += expect_uint("reply within window: ready", (unsigned int)s.answer_ready, 1);

	/* Query lost: tick at the boundary triggers one retransmit. */
	sim_start(&s, 1000);
	sim_tick(&s, 1199);                       /* one tick before timeout */
	failures += expect_uint("just before timeout: 0 retransmits",
	                        s.retransmits, 0);
	sim_tick(&s, 1200);                       /* exactly at timeout */
	failures += expect_uint("at timeout: 1 retransmit", s.retransmits, 1);
	failures += expect_uint("at timeout: retries = 1",  s.send_retries, 1);

	/* Burst of ticks during the new wait window: no extra retransmits. */
	for (unsigned int t = 1201; t < 1400; t += 10) sim_tick(&s, t);
	failures += expect_uint("mid-window burst: still 1 retransmit",
	                        s.retransmits, 1);

	/* Total wipeout: every reply lost, must give up after MAX retries. */
	sim_start(&s, 1000);
	for (unsigned int t = 1000; t < 5000; t += 10) sim_tick(&s, t);
	failures += expect_uint("total wipeout: exactly MAX retransmits",
	                        s.retransmits, DNS_MAX_RETRIES);
	failures += expect_uint("after MAX: slot inert",      s.last_send_tick, 0);
	/* Subsequent ticks must not retransmit again. */
	unsigned int rt_after = s.retransmits;
	for (unsigned int t = 5000; t < 10000; t += 10) sim_tick(&s, t);
	failures += expect_uint("after inert: no more retransmits",
	                        s.retransmits, rt_after);

	/* Reply landing mid-retry sequence stops the pump. */
	sim_start(&s, 1000);
	sim_tick(&s, 1200);                       /* 1st retransmit */
	sim_tick(&s, 1400);                       /* 2nd retransmit */
	sim_on_reply(&s);                         /* reply lands */
	for (unsigned int t = 1500; t < 5000; t += 10) sim_tick(&s, t);
	failures += expect_uint("reply mid-retry: 2 retransmits total",
	                        s.retransmits, 2);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
