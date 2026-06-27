/* Host-side test for RFC 5961 §4 "SYN-in-window" handling added to
 * tcp_handle in kernel/drivers/net/tcp.c. A SYN arriving on an already-
 * synchronized connection MUST NOT reset state -- it gets a challenge
 * ACK and the state stays as-is. Without this, an attacker (or a stray
 * retransmit from a crashed-and-rebooted peer) could blow away a live
 * connection by spoofing a 5-tuple match with the SYN flag set.
 *
 * Decision (mirrors kernel):
 *   - SYN bit clear: no special handling, fall through to normal switch.
 *   - SYN bit set AND state in {LISTEN, SYN_SENT, SYN_RECEIVED}: fall
 *     through (legitimate handshake path).
 *   - SYN bit set AND state synchronized (ESTABLISHED and beyond):
 *     send a pure ACK (challenge), drop the SYN, state unchanged.
 */
#include <stdio.h>

enum {
	CLOSED       = 0,
	LISTEN       = 1,
	SYN_SENT     = 2,
	SYN_RECEIVED = 3,
	ESTABLISHED  = 4,
	FIN_WAIT_1   = 5,
	FIN_WAIT_2   = 6,
	CLOSE_WAIT   = 7,
	CLOSING      = 8,
	LAST_ACK     = 9,
	TIME_WAIT    = 10,
};

#define TCP_SYN 0x02
#define TCP_ACK 0x10

struct sim {
	int state;
	int challenges_sent;     /* incremented per challenge ACK */
	int state_perturbed;     /* set if the SYN would have changed state */
};

/* Mirror of the rule. Returns 1 if the caller should `return` (the SYN
 * was challenged and the rest of the dispatch is skipped). */
static int syn_in_window(struct sim *s, unsigned char flags)
{
	if (!(flags & TCP_SYN))
		return 0;
	if (s->state == LISTEN || s->state == SYN_SENT || s->state == SYN_RECEIVED)
		return 0;
	/* Synchronized state: challenge + drop. */
	s->challenges_sent++;
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

	/* No SYN flag: rule doesn't fire regardless of state. */
	s.state = ESTABLISHED; s.challenges_sent = 0;
	failures += expect_int("no SYN: rule doesn't fire",       syn_in_window(&s, TCP_ACK), 0);
	failures += expect_int("no SYN: no challenge",             s.challenges_sent,         0);

	/* SYN in LISTEN/SYN_SENT/SYN_RECEIVED: handshake path, no challenge. */
	s.state = LISTEN; s.challenges_sent = 0;
	failures += expect_int("SYN in LISTEN: rule skips",        syn_in_window(&s, TCP_SYN), 0);
	failures += expect_int("SYN in LISTEN: no challenge",      s.challenges_sent,         0);

	s.state = SYN_SENT; s.challenges_sent = 0;
	failures += expect_int("SYN in SYN_SENT: rule skips",      syn_in_window(&s, TCP_SYN | TCP_ACK), 0);
	failures += expect_int("SYN in SYN_SENT: no challenge",    s.challenges_sent,         0);

	s.state = SYN_RECEIVED; s.challenges_sent = 0;
	failures += expect_int("SYN in SYN_RECV: rule skips",      syn_in_window(&s, TCP_SYN), 0);

	/* SYN in synchronized state: challenge + drop. State must NOT change
	 * here -- the simulator only checks that the return value indicates
	 * the caller should not fall through. */
	s.state = ESTABLISHED; s.challenges_sent = 0;
	failures += expect_int("SYN in ESTABLISHED: challenged",   syn_in_window(&s, TCP_SYN), 1);
	failures += expect_int("SYN in ESTABLISHED: 1 challenge",  s.challenges_sent,         1);
	failures += expect_int("SYN in ESTABLISHED: state kept",   s.state,                   ESTABLISHED);

	/* Same for every other synchronized state. */
	int sync_states[] = { FIN_WAIT_1, FIN_WAIT_2, CLOSE_WAIT, CLOSING, LAST_ACK, TIME_WAIT };
	for (unsigned i = 0; i < sizeof(sync_states) / sizeof(sync_states[0]); i++) {
		s.state = sync_states[i]; s.challenges_sent = 0;
		char label[64];
		snprintf(label, sizeof(label), "SYN in state %d: challenged", sync_states[i]);
		failures += expect_int(label, syn_in_window(&s, TCP_SYN), 1);
	}

	/* Burst of SYNs: each generates a fresh challenge ACK (the kernel
	 * doesn't rate-limit them at this layer -- ICMP unreach has its own
	 * limiter, TCP challenges are cheap and the legitimate use case is a
	 * one-off retransmit). */
	s.state = ESTABLISHED; s.challenges_sent = 0;
	for (int i = 0; i < 10; i++) syn_in_window(&s, TCP_SYN);
	failures += expect_int("10 SYNs: 10 challenges",           s.challenges_sent,         10);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
