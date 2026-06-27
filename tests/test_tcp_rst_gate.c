/* Host-side test for the RST acceptance gate added to tcp_handle in
 * kernel/drivers/net/tcp.c. Without it, an attacker who can spoof the
 * connection's 5-tuple can blind-RST any in-progress connection just by
 * sending a single TCP RST packet -- no need to guess seq.
 *
 * RFC 5961 §3 policy implemented here:
 *   - Established / FIN_WAIT* / CLOSE_WAIT / LAST_ACK / TIME_WAIT:
 *     RST is acted on ONLY if seg_seq == rcv_nxt. Anything else is
 *     silently dropped (RFC suggests sending a challenge ACK; we just
 *     drop -- the attacker learns nothing either way).
 *   - SYN_SENT / SYN_RECEIVED / LISTEN: rcv_nxt is meaningless before
 *     the SYN handshake completes, so we still accept blindly. The
 *     existing RFC 793 rules for ACK acceptance limit this.
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

struct tcb {
	int          state;
	int          in_use;
	unsigned int rcv_nxt;
};

/* Mirror of the RST gate -- returns 1 if the TCB transitioned to CLOSED. */
static int rst_handle(struct tcb *t, unsigned int seg_seq)
{
	int strict = (t->state != SYN_SENT &&
	              t->state != SYN_RECEIVED &&
	              t->state != LISTEN);
	if (!strict || seg_seq == t->rcv_nxt) {
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

	/* ESTABLISHED with rcv_nxt=1000: RST at seq=1000 wins. */
	t.state = ESTABLISHED; t.in_use = 1; t.rcv_nxt = 1000;
	failures += expect_int("EST + matching seq: accepted",  rst_handle(&t, 1000), 1);
	failures += expect_int("EST + matching seq: state=CLOSED", t.state,           CLOSED);

	/* ESTABLISHED with rcv_nxt=1000: RST at seq=999 (off by one) dropped. */
	t.state = ESTABLISHED; t.in_use = 1; t.rcv_nxt = 1000;
	failures += expect_int("EST + off-by-one seq: dropped", rst_handle(&t, 999), 0);
	failures += expect_int("EST + off-by-one seq: state kept", t.state,            ESTABLISHED);

	/* ESTABLISHED + far-off attacker seq: dropped. */
	t.state = ESTABLISHED; t.in_use = 1; t.rcv_nxt = 1000;
	failures += expect_int("EST + spoofed seq: dropped",    rst_handle(&t, 0xDEADBEEFu), 0);
	failures += expect_int("EST + spoofed seq: still in_use", t.in_use,                  1);

	/* FIN_WAIT_2 same rule. */
	t.state = FIN_WAIT_2; t.in_use = 1; t.rcv_nxt = 500;
	failures += expect_int("FIN_WAIT_2 + matching: accepted", rst_handle(&t, 500), 1);

	t.state = LAST_ACK; t.in_use = 1; t.rcv_nxt = 500;
	failures += expect_int("LAST_ACK + wrong seq: dropped",   rst_handle(&t, 0),   0);
	failures += expect_int("LAST_ACK + wrong seq: state kept", t.state,            LAST_ACK);

	/* SYN_SENT: rcv_nxt not yet meaningful, RST always accepted. */
	t.state = SYN_SENT; t.in_use = 1; t.rcv_nxt = 0;
	failures += expect_int("SYN_SENT + any seq: accepted",   rst_handle(&t, 0xCAFEBABEu), 1);

	/* SYN_RECEIVED: same. */
	t.state = SYN_RECEIVED; t.in_use = 1; t.rcv_nxt = 0;
	failures += expect_int("SYN_RECV + any seq: accepted",   rst_handle(&t, 12345),       1);

	/* LISTEN: same (pre-handshake). */
	t.state = LISTEN; t.in_use = 1; t.rcv_nxt = 0;
	failures += expect_int("LISTEN + any seq: accepted",     rst_handle(&t, 1),           1);

	/* TIME_WAIT also subject to strict gate. */
	t.state = TIME_WAIT; t.in_use = 1; t.rcv_nxt = 8000;
	failures += expect_int("TIME_WAIT + matching: accepted", rst_handle(&t, 8000), 1);
	t.state = TIME_WAIT; t.in_use = 1; t.rcv_nxt = 8000;
	failures += expect_int("TIME_WAIT + wrong: dropped",     rst_handle(&t, 7999), 0);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
