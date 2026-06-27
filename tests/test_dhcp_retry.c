/* Host-side test for the DHCP retransmit pump in
 * kernel/drivers/net/dhcp.c (the dhcp_tick function). Policy:
 *
 *   - Active only in DHCP_DISCOVERING or DHCP_REQUESTING; every other
 *     state is a no-op.
 *   - Re-broadcasts the current message (DISCOVER vs REQUEST per state)
 *     after DHCP_RETRY_TICKS (400 ticks = 4 s) of silence.
 *   - Caps retries at DHCP_MAX_RETRIES (4). One more failure transitions
 *     state to DHCP_FAILED so userland stops polling on a dead lease.
 *   - On the DISCOVERING -> REQUESTING transition (receiving an OFFER),
 *     the retry budget resets so the REQUEST phase gets its own MAX.
 *
 * Without this, a single lost DISCOVER used to leave the client stuck
 * in DISCOVERING forever and net_poll_once never recovered.
 */
#include <stdio.h>

#define DHCP_RETRY_TICKS  400u
#define DHCP_MAX_RETRIES  4u

enum { INIT, DISCOVERING, REQUESTING, BOUND, FAILED };

struct sim {
	int           state;
	unsigned int  last_send;
	unsigned int  retries;
	unsigned int  discover_sends;
	unsigned int  request_sends;
};

static void sim_start(struct sim *s, unsigned int now)
{
	s->state          = DISCOVERING;
	s->last_send      = now;
	s->retries        = 0;
	s->discover_sends = 1;  /* the kick-off DISCOVER in dhcp_start */
	s->request_sends  = 0;
}

static void sim_receive_offer(struct sim *s, unsigned int now)
{
	s->state         = REQUESTING;
	s->last_send     = now;
	s->retries       = 0;
	s->request_sends = 1;   /* the immediate REQUEST after OFFER */
}

static void sim_receive_ack(struct sim *s)
{
	s->state = BOUND;
}

/* Mirror of dhcp_tick. */
static void sim_tick(struct sim *s, unsigned int now)
{
	if (s->state != DISCOVERING && s->state != REQUESTING)
		return;
	if ((now - s->last_send) < DHCP_RETRY_TICKS)
		return;
	if (s->retries >= DHCP_MAX_RETRIES) {
		s->state = FAILED;
		return;
	}
	if (s->state == DISCOVERING)
		s->discover_sends++;
	else
		s->request_sends++;
	s->last_send = now;
	s->retries++;
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

	/* No-op outside the active states. */
	s.state = INIT; s.last_send = 0; s.retries = 0;
	s.discover_sends = s.request_sends = 0;
	for (unsigned int t = 0; t < 5000; t += 100) sim_tick(&s, t);
	failures += expect_uint("INIT: no retransmits",  s.discover_sends, 0);

	s.state = BOUND;
	for (unsigned int t = 0; t < 5000; t += 100) sim_tick(&s, t);
	failures += expect_uint("BOUND: no retransmits", s.discover_sends, 0);

	/* Healthy path: DISCOVER -> OFFER (mid-window) -> REQUEST -> ACK.
	 * No retransmits anywhere. */
	sim_start(&s, 1000);
	sim_tick(&s, 1300);                    /* within first window */
	sim_receive_offer(&s, 1300);
	sim_tick(&s, 1600);                    /* within REQUEST window */
	sim_receive_ack(&s);
	failures += expect_int("healthy: state = BOUND",          s.state,          BOUND);
	failures += expect_uint("healthy: 1 DISCOVER total",     s.discover_sends, 1);
	failures += expect_uint("healthy: 1 REQUEST total",      s.request_sends,  1);

	/* DISCOVER lost: tick at 400-tick boundary fires retransmit. */
	sim_start(&s, 1000);
	sim_tick(&s, 1399);
	failures += expect_uint("DISCOVER t+399: no retransmit", s.discover_sends, 1);
	sim_tick(&s, 1400);
	failures += expect_uint("DISCOVER t+400: 2 sends",       s.discover_sends, 2);
	failures += expect_uint("DISCOVER t+400: retries=1",     s.retries,        1);

	/* Total wipeout of OFFER replies: state goes FAILED after MAX. */
	sim_start(&s, 1000);
	for (unsigned int t = 1000; t < 10000; t += 100) sim_tick(&s, t);
	failures += expect_uint("wipeout: DISCOVER sends = 1 + MAX",
	                        s.discover_sends, 1u + DHCP_MAX_RETRIES);
	failures += expect_int("wipeout: state = FAILED",         s.state, FAILED);

	/* OFFER arrives DURING the retry sequence: retry budget resets for
	 * the REQUEST phase. */
	sim_start(&s, 1000);
	sim_tick(&s, 1400);                    /* 1 retransmit */
	sim_tick(&s, 1800);                    /* 2 retransmits */
	sim_receive_offer(&s, 1900);
	failures += expect_uint("OFFER mid-retry: retries reset", s.retries, 0);

	/* Now REQUEST phase suffers losses: a fresh MAX of retransmits. */
	sim_tick(&s, 2300);                    /* 1 retransmit of REQUEST */
	sim_tick(&s, 2700);                    /* 2 */
	failures += expect_uint("REQUEST: 3 sends total",         s.request_sends, 3);

	/* Subsequent ticks within window: no extra. */
	sim_tick(&s, 2750);
	failures += expect_uint("REQUEST mid-window: still 3",    s.request_sends, 3);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
