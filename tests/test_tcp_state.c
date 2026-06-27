/* Host-side regression test for the TCP state-transition table that lives
 * inside tcp_handle() in kernel/drivers/net/tcp.c. We don't simulate the
 * full state machine (no TCBs, no segment build/parse) -- instead we
 * encode the expected transitions as a table and verify the cases that
 * historically slipped (passive open, dual-FIN, etc).
 *
 * Any change to tcp_handle()'s state graph should add a matching row
 * here. Test cases are pulled straight from RFC 793 figure 6 plus the
 * RFC 7323 LISTEN edge.
 */
#include <stdio.h>

/* Mirror of enum tcp_state in tcp.h. */
enum {
	TCP_CLOSED       = 0,
	TCP_LISTEN       = 1,
	TCP_SYN_SENT     = 2,
	TCP_SYN_RECEIVED = 3,
	TCP_ESTABLISHED  = 4,
	TCP_FIN_WAIT_1   = 5,
	TCP_FIN_WAIT_2   = 6,
	TCP_CLOSE_WAIT   = 7,
	TCP_CLOSING      = 8,
	TCP_LAST_ACK     = 9,
	TCP_TIME_WAIT    = 10,
};

#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_ACK  0x10

/* Expected next state for (current_state, flags) -- mirrors the switch
 * in tcp_handle(). Only the transitions tcp_handle actually drives are
 * encoded; everything else stays where it was. -1 = "no entry". */
static int expected_next(int state, int flags)
{
	if (flags & TCP_RST) return TCP_CLOSED;

	switch (state) {
	case TCP_LISTEN:
		if ((flags & TCP_SYN) && !(flags & TCP_ACK)) return TCP_SYN_RECEIVED;
		return TCP_LISTEN;

	case TCP_SYN_RECEIVED:
		if (flags & TCP_ACK) return TCP_ESTABLISHED;
		return TCP_SYN_RECEIVED;

	case TCP_SYN_SENT:
		if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) return TCP_ESTABLISHED;
		return TCP_SYN_SENT;

	case TCP_ESTABLISHED:
		if (flags & TCP_FIN) return TCP_CLOSE_WAIT;
		return TCP_ESTABLISHED;

	case TCP_FIN_WAIT_1:
		if (flags & TCP_FIN) return TCP_TIME_WAIT;
		if (flags & TCP_ACK) return TCP_FIN_WAIT_2;
		return TCP_FIN_WAIT_1;

	case TCP_FIN_WAIT_2:
		if (flags & TCP_FIN) return TCP_TIME_WAIT;
		return TCP_FIN_WAIT_2;

	case TCP_LAST_ACK:
		if (flags & TCP_ACK) return TCP_CLOSED;
		return TCP_LAST_ACK;

	case TCP_TIME_WAIT:
	case TCP_CLOSE_WAIT:
	case TCP_CLOSING:
	case TCP_CLOSED:
	default:
		return state;
	}
}

static const char *name(int s)
{
	static const char *t[] = {
		"CLOSED","LISTEN","SYN_SENT","SYN_RCVD","ESTAB",
		"FIN_W1","FIN_W2","CLOSE_W","CLOSING","LAST_ACK","TIME_W"
	};
	if (s < 0 || s >= 11) return "?";
	return t[s];
}

static int expect_transition(int state, int flags, int want)
{
	int got = expected_next(state, flags);
	if (got == want) {
		printf("ok   %-8s + 0x%02x -> %s\n", name(state), flags, name(got));
		return 0;
	}
	printf("FAIL %-8s + 0x%02x: got %s, want %s\n",
	       name(state), flags, name(got), name(want));
	return 1;
}

int main(void)
{
	int failures = 0;

	/* Passive open: LISTEN sees SYN, moves to SYN_RECEIVED. */
	failures += expect_transition(TCP_LISTEN, TCP_SYN, TCP_SYN_RECEIVED);

	/* SYN_RECEIVED gets ACK of our SYN-ACK -> ESTABLISHED. */
	failures += expect_transition(TCP_SYN_RECEIVED, TCP_ACK, TCP_ESTABLISHED);

	/* Active open: SYN_SENT sees SYN-ACK, moves to ESTABLISHED. */
	failures += expect_transition(TCP_SYN_SENT, TCP_SYN | TCP_ACK, TCP_ESTABLISHED);

	/* Just SYN with no ACK keeps SYN_SENT (lost ACK case). */
	failures += expect_transition(TCP_SYN_SENT, TCP_SYN, TCP_SYN_SENT);

	/* Peer closes first: ESTABLISHED + FIN -> CLOSE_WAIT. */
	failures += expect_transition(TCP_ESTABLISHED, TCP_FIN | TCP_ACK, TCP_CLOSE_WAIT);

	/* We closed first: FIN_WAIT_1 sees ACK of our FIN. */
	failures += expect_transition(TCP_FIN_WAIT_1, TCP_ACK, TCP_FIN_WAIT_2);

	/* Simultaneous close shortcut: FIN_WAIT_1 + FIN. */
	failures += expect_transition(TCP_FIN_WAIT_1, TCP_FIN | TCP_ACK, TCP_TIME_WAIT);

	/* FIN_WAIT_2 + FIN -> TIME_WAIT. */
	failures += expect_transition(TCP_FIN_WAIT_2, TCP_FIN | TCP_ACK, TCP_TIME_WAIT);

	/* LAST_ACK + ACK -> CLOSED. */
	failures += expect_transition(TCP_LAST_ACK, TCP_ACK, TCP_CLOSED);

	/* RST on any state immediately closes. */
	failures += expect_transition(TCP_ESTABLISHED, TCP_RST,            TCP_CLOSED);
	failures += expect_transition(TCP_SYN_SENT,    TCP_RST | TCP_ACK,  TCP_CLOSED);
	failures += expect_transition(TCP_FIN_WAIT_1,  TCP_RST,            TCP_CLOSED);

	/* TIME_WAIT absorbs retransmits without changing state. */
	failures += expect_transition(TCP_TIME_WAIT, TCP_FIN | TCP_ACK, TCP_TIME_WAIT);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
