/* Host-side test for the zero-window persist probe in
 * kernel/drivers/net/tcp.c (the persist_armed branch added to
 * tcp_drain_tx + tcp_tick). RFC 1122 §4.2.2.17 policy:
 *
 *   - drain_tx with snd_wnd == 0 and tx_pending > 0 arms persist with
 *     the initial interval (TCP_PERSIST_INITIAL = 100 ticks = 1 s).
 *   - tick fires a 1-byte probe when (now - persist_last_tick) >=
 *     persist_interval, then doubles persist_interval (capped at
 *     TCP_PERSIST_MAX = 6000 ticks = 60 s).
 *   - An ACK that announces snd_wnd > 0 clears persist_armed so the
 *     drain loop can resume.
 *
 * The bug this prevents: the old drain_tx fallback `snd_wnd ? : avail`
 * sent local-buffer bytes through a peer that just told us it has zero
 * receive buffer, overrunning their stack.
 */
#include <stdio.h>

#define TCP_PERSIST_INITIAL 100u
#define TCP_PERSIST_MAX     6000u

struct sim {
	unsigned int snd_wnd;          /* peer-advertised window */
	unsigned int tx_pending;       /* bytes queued locally */
	unsigned int in_flight;
	unsigned char persist_armed;
	unsigned int  persist_last_tick;
	unsigned int  persist_interval;
	unsigned int  probes_sent;
};

static void sim_reset(struct sim *s)
{
	s->snd_wnd           = 4096;
	s->tx_pending        = 0;
	s->in_flight         = 0;
	s->persist_armed     = 0;
	s->persist_last_tick = 0;
	s->persist_interval  = TCP_PERSIST_INITIAL;
	s->probes_sent       = 0;
}

/* Mirror of the new drain_tx persist-arm branch (the part that runs at
 * the top of the loop). Returns 0 if the loop would `return` (no send),
 * 1 if it would proceed to data emission. */
static int sim_drain_check(struct sim *s, unsigned int now)
{
	if (s->tx_pending == 0) return 0;
	if (s->snd_wnd == 0 && s->in_flight == 0) {
		if (!s->persist_armed) {
			s->persist_armed     = 1;
			s->persist_last_tick = now;
			s->persist_interval  = TCP_PERSIST_INITIAL;
		}
		return 0;
	}
	return 1;
}

/* Mirror of the tcp_tick persist branch. */
static void sim_tick(struct sim *s, unsigned int now)
{
	if (s->persist_armed && s->tx_pending > 0 &&
	    (now - s->persist_last_tick) >= s->persist_interval) {
		s->probes_sent++;
		s->persist_last_tick = now;
		s->persist_interval *= 2u;
		if (s->persist_interval > TCP_PERSIST_MAX)
			s->persist_interval = TCP_PERSIST_MAX;
	}
}

/* Mirror of the ACK-side persist clear. */
static void sim_on_ack(struct sim *s, unsigned int new_snd_wnd)
{
	s->snd_wnd = new_snd_wnd;
	if (s->snd_wnd != 0)
		s->persist_armed = 0;
}

static int expect_uint(const char *label, unsigned int got, unsigned int want)
{
	if (got == want) { printf("ok   %-50s = %u\n", label, got); return 0; }
	printf("FAIL %s: got %u, want %u\n", label, got, want);
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
	struct sim s;

	/* Open window: drain proceeds, persist stays disarmed. */
	sim_reset(&s);
	s.tx_pending = 500;
	failures += expect_int("open window: drain proceeds", sim_drain_check(&s, 100), 1);
	failures += expect_uint("open window: persist disarmed", s.persist_armed, 0);

	/* Zero window with tx_pending: persist arms with initial interval. */
	sim_reset(&s);
	s.snd_wnd    = 0;
	s.tx_pending = 500;
	failures += expect_int("zero window: drain skips",   sim_drain_check(&s, 1000), 0);
	failures += expect_uint("zero window: persist armed", s.persist_armed, 1);
	failures += expect_uint("zero window: interval reset", s.persist_interval, TCP_PERSIST_INITIAL);
	failures += expect_uint("zero window: last_tick = now", s.persist_last_tick, 1000);

	/* Re-entering drain before persist fires must NOT reset the timer
	 * (otherwise persist would never elapse under repeated send calls). */
	sim_drain_check(&s, 1050);
	failures += expect_uint("re-arm idempotent: last_tick unchanged",
	                        s.persist_last_tick, 1000);

	/* Tick before interval elapses: no probe. */
	sim_tick(&s, 1050);
	failures += expect_uint("tick t+50: no probe yet", s.probes_sent, 0);

	/* Tick at exactly interval: probe goes out, interval doubles. */
	sim_tick(&s, 1100);
	failures += expect_uint("tick t+100: 1 probe sent", s.probes_sent,    1);
	failures += expect_uint("after probe: interval = 200", s.persist_interval, 200);

	/* Subsequent ticks scale: each probe at the new doubled interval. */
	sim_tick(&s, 1300);       /* 200 ticks after last probe */
	failures += expect_uint("tick t+300: 2nd probe", s.probes_sent,    2);
	failures += expect_uint("after 2nd: interval = 400", s.persist_interval, 400);

	/* Interval saturates at TCP_PERSIST_MAX after enough doublings.
	 * Starting at 400 -> 800 -> 1600 -> 3200 -> 6400 capped to 6000. */
	for (int i = 0; i < 10; i++) {
		sim_tick(&s, s.persist_last_tick + s.persist_interval);
	}
	failures += expect_uint("interval saturates at MAX", s.persist_interval, TCP_PERSIST_MAX);

	/* Peer reopens window: persist clears. */
	sim_on_ack(&s, 4096);
	failures += expect_uint("window reopen: persist cleared",  s.persist_armed, 0);
	failures += expect_uint("window reopen: snd_wnd updated",  s.snd_wnd,       4096);

	/* And drain now proceeds normally. */
	failures += expect_int("after reopen: drain proceeds", sim_drain_check(&s, 5000), 1);

	/* Zero window but in_flight > 0: persist arm-branch is SKIPPED
	 * (we have outstanding data; the RTO + dup-ACK paths handle it).
	 * The actual data send is stopped a few lines later in drain_tx by
	 * the `window <= in_flight` guard, which is not modelled here --
	 * what matters at THIS layer is that persist stays disarmed so the
	 * RTO retransmit owns recovery. */
	sim_reset(&s);
	s.snd_wnd    = 0;
	s.tx_pending = 500;
	s.in_flight  = 100;
	failures += expect_int("zero wnd + in_flight: arm-branch skipped", sim_drain_check(&s, 100), 1);
	failures += expect_uint("zero wnd + in_flight: NO persist arm",    s.persist_armed, 0);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
