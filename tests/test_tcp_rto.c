/* Host-side test for the TCP retransmit-timer state machine in
 * kernel/drivers/net/tcp.c. The RTO loop:
 *   - if nothing in flight: clear last_seg_ticks, no retransmit
 *   - if (now - last_seg_ticks) < rto_ticks: still waiting, skip
 *   - else if retries >= TCP_MAX_RETRIES: drop the connection
 *   - else: retransmit, retries++, rto_ticks *= 2 (capped at MAX),
 *           last_seg_ticks = now
 *
 * The doubling-then-cap is exactly the kind of arithmetic that hides
 * regressions: forgetting the cap turns RTO into an absurd number;
 * forgetting the doubling shrinks the retry envelope from ~31 s to
 * a tight ~5 s. Both are silent until packet loss on a real link.
 */
#include <stdio.h>

#define TCP_RTO_INITIAL  100u
#define TCP_RTO_MAX      6000u
#define TCP_MAX_RETRIES  5u

enum tcp_state { TCP_ESTABLISHED, TCP_CLOSED };

struct tcb_sim {
	int             state;
	int             in_use;
	unsigned int    snd_una, snd_nxt;
	unsigned int    last_seg_ticks;
	unsigned int    rto_ticks;
	unsigned int    retries;
	int             retransmit_count;
};

enum tick_result { TICK_NOOP, TICK_RETRANSMIT, TICK_DROP };

static int rto_step(struct tcb_sim *t, unsigned int now)
{
	if (t->last_seg_ticks == 0) return TICK_NOOP;
	if (t->snd_una == t->snd_nxt) {
		t->last_seg_ticks = 0;
		return TICK_NOOP;
	}
	if ((now - t->last_seg_ticks) < t->rto_ticks) return TICK_NOOP;
	if (t->retries >= TCP_MAX_RETRIES) {
		t->state  = TCP_CLOSED;
		t->in_use = 0;
		return TICK_DROP;
	}
	t->retransmit_count++;
	t->retries++;
	t->rto_ticks *= 2;
	if (t->rto_ticks > TCP_RTO_MAX) t->rto_ticks = TCP_RTO_MAX;
	t->last_seg_ticks = now;
	return TICK_RETRANSMIT;
}

static int expect_u(const char *label, unsigned int got, unsigned int want)
{
	if (got == want) { printf("ok   %-55s = %u\n", label, got); return 0; }
	printf("FAIL %s: got %u, want %u\n", label, got, want);
	return 1;
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
	struct tcb_sim t;

	/* --- nothing in flight: noop, last_seg cleared --------------- */
	t = (struct tcb_sim){ TCP_ESTABLISHED, 1, 1000, 1000, 50, TCP_RTO_INITIAL, 0, 0 };
	failures += expect_int("snd_una == snd_nxt -> noop",
	                       rto_step(&t, 200), TICK_NOOP);
	failures += expect_u("noop clears last_seg_ticks",
	                     t.last_seg_ticks, 0);

	/* --- last_seg_ticks==0: noop without touching anything ------- */
	t = (struct tcb_sim){ TCP_ESTABLISHED, 1, 100, 200, 0, TCP_RTO_INITIAL, 0, 0 };
	failures += expect_int("last_seg_ticks==0 -> noop",
	                       rto_step(&t, 5000), TICK_NOOP);
	failures += expect_u("noop preserves rto_ticks",
	                     t.rto_ticks, TCP_RTO_INITIAL);

	/* --- pre-RTO: still waiting, no retransmit ------------------- */
	t = (struct tcb_sim){ TCP_ESTABLISHED, 1, 100, 200, 1000, TCP_RTO_INITIAL, 0, 0 };
	failures += expect_int("now < last+rto -> noop",
	                       rto_step(&t, 1099), TICK_NOOP);
	failures += expect_u("pre-RTO retries unchanged", t.retries, 0);

	/* --- first RTO fire: retransmit, retries=1, rto doubles ----- */
	failures += expect_int("now == last+rto -> retransmit",
	                       rto_step(&t, 1100), TICK_RETRANSMIT);
	failures += expect_u("retries 0 -> 1", t.retries, 1);
	failures += expect_u("rto doubled to 200", t.rto_ticks, 200);
	failures += expect_u("last_seg_ticks bumped to now", t.last_seg_ticks, 1100);

	/* --- subsequent fires: keep doubling until cap. ------------- */
	t = (struct tcb_sim){ TCP_ESTABLISHED, 1, 100, 200, 1000, TCP_RTO_INITIAL, 0, 0 };
	unsigned int now = 1000;
	for (int i = 1; i <= 5; i++) {
		now += t.rto_ticks;
		(void)rto_step(&t, now);
		if (i == 1) failures += expect_u("retry#1 rto=200", t.rto_ticks, 200);
		if (i == 2) failures += expect_u("retry#2 rto=400", t.rto_ticks, 400);
		if (i == 3) failures += expect_u("retry#3 rto=800", t.rto_ticks, 800);
		if (i == 4) failures += expect_u("retry#4 rto=1600", t.rto_ticks, 1600);
		if (i == 5) failures += expect_u("retry#5 rto=3200", t.rto_ticks, 3200);
	}
	failures += expect_u("after 5 retries retries==5", t.retries, 5);
	failures += expect_u("retransmit_count==5",       (unsigned)t.retransmit_count, 5);

	/* --- 6th tick: retries >= MAX -> drop the conn -------------- */
	now += t.rto_ticks;
	failures += expect_int("retries==MAX -> drop", rto_step(&t, now), TICK_DROP);
	failures += expect_int("dropped: state=CLOSED", t.state,  TCP_CLOSED);
	failures += expect_int("dropped: in_use=0",    t.in_use, 0);

	/* --- cap: a large initial RTO doubles into the cap and stays. */
	t = (struct tcb_sim){ TCP_ESTABLISHED, 1, 100, 200, 0, 4000, 0, 0 };
	t.last_seg_ticks = 1000;
	(void)rto_step(&t, 1000 + 4000);
	failures += expect_u("4000 doubled -> 6000 cap",  t.rto_ticks, TCP_RTO_MAX);
	t.last_seg_ticks = 100000;
	(void)rto_step(&t, 100000 + TCP_RTO_MAX);
	failures += expect_u("6000 doubled -> stays 6000", t.rto_ticks, TCP_RTO_MAX);

	/* --- ack drains snd_una to snd_nxt: timer disarms ----------- */
	t = (struct tcb_sim){ TCP_ESTABLISHED, 1, 100, 200, 50, TCP_RTO_INITIAL, 3, 0 };
	t.snd_una = t.snd_nxt;
	(void)rto_step(&t, 1000);
	failures += expect_u("ack drains: last_seg_ticks cleared",
	                     t.last_seg_ticks, 0);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
