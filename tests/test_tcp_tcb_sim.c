/* Full-TCB simulation that walks a single connection from active open
 * through data exchange to clean close, asserting that the per-byte
 * counters (snd_una/snd_nxt, rcv_nxt) and the cumulative state pointer
 * stay consistent with the segment stream the peer emits.
 *
 * Not a wire-level test -- we don't build/parse real TCP headers. The
 * goal is to lock the SEQ/ACK arithmetic the kernel relies on:
 *
 *   - SYN consumes one sequence number (snd_nxt += 1 after the SYN).
 *   - FIN consumes one sequence number.
 *   - In ESTABLISHED, snd_nxt advances by data_len on send;
 *     snd_una advances by ACK on receive (capped at snd_nxt).
 *   - rcv_nxt advances by data_len for in-order data; FIN bumps it +1.
 *
 * Any silent drift in those rules would corrupt every retransmit /
 * SACK decision the live kernel makes.
 */
#include <stdio.h>

enum {
	S_CLOSED, S_SYN_SENT, S_ESTABLISHED,
	S_FIN_WAIT_1, S_FIN_WAIT_2, S_TIME_WAIT,
};

struct tcb {
	int          state;
	unsigned int snd_una;
	unsigned int snd_nxt;
	unsigned int rcv_nxt;
};

static void active_open(struct tcb *t, unsigned int iss)
{
	t->snd_una = iss;
	t->snd_nxt = iss + 1;   /* SYN consumes one seq */
	t->state   = S_SYN_SENT;
}

static void recv_synack(struct tcb *t, unsigned int peer_iss, unsigned int ack)
{
	if (t->state != S_SYN_SENT) return;
	if (ack != t->snd_nxt) return;
	t->snd_una = ack;
	t->rcv_nxt = peer_iss + 1;  /* peer SYN consumes one */
	t->state   = S_ESTABLISHED;
}

static void send_data(struct tcb *t, unsigned int n)
{
	if (t->state != S_ESTABLISHED) return;
	t->snd_nxt += n;
}

static void recv_ack(struct tcb *t, unsigned int ack)
{
	if (t->state != S_ESTABLISHED && t->state != S_FIN_WAIT_1) return;
	if (ack > t->snd_nxt) return;       /* future ack -- drop */
	if (ack > t->snd_una) t->snd_una = ack;
}

static void recv_data(struct tcb *t, unsigned int seq, unsigned int n)
{
	if (t->state != S_ESTABLISHED) return;
	if (seq != t->rcv_nxt) return;      /* OOO not modelled here */
	t->rcv_nxt += n;
}

static void close_active(struct tcb *t)
{
	if (t->state != S_ESTABLISHED) return;
	t->snd_nxt += 1;                    /* FIN consumes one seq */
	t->state    = S_FIN_WAIT_1;
}

static void recv_fin_ack(struct tcb *t, unsigned int ack)
{
	/* In FIN_WAIT_1 the peer's ACK of our FIN moves to FIN_WAIT_2. */
	if (t->state == S_FIN_WAIT_1 && ack == t->snd_nxt)
		t->state = S_FIN_WAIT_2;
}

static void recv_peer_fin(struct tcb *t, unsigned int seq)
{
	if (t->state != S_FIN_WAIT_2) return;
	if (seq != t->rcv_nxt) return;
	t->rcv_nxt += 1;                    /* peer FIN consumes one */
	t->state    = S_TIME_WAIT;
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
	struct tcb t = {0};

	/* --- active open ----------------------------------------------------- */
	active_open(&t, 1000);
	failures += expect_int("open: state = SYN_SENT", t.state, S_SYN_SENT);
	failures += expect_uint("open: snd_una = 1000",  t.snd_una, 1000);
	failures += expect_uint("open: snd_nxt = 1001 (SYN consumes 1)",
	                        t.snd_nxt, 1001);

	/* --- peer SYN-ACK ---------------------------------------------------- */
	recv_synack(&t, 5000, 1001);
	failures += expect_int("synack: state = ESTABLISHED", t.state, S_ESTABLISHED);
	failures += expect_uint("synack: snd_una advanced to ack", t.snd_una, 1001);
	failures += expect_uint("synack: rcv_nxt = peer_iss + 1",  t.rcv_nxt, 5001);

	/* Mismatched SYN-ACK ack is ignored (defence). */
	{
		struct tcb tt = {0};
		active_open(&tt, 2000);
		recv_synack(&tt, 9000, 999);    /* wrong ack */
		failures += expect_int("bad synack: stays SYN_SENT", tt.state, S_SYN_SENT);
		failures += expect_uint("bad synack: rcv_nxt untouched", tt.rcv_nxt, 0);
	}

	/* --- data exchange --------------------------------------------------- */
	send_data(&t, 100);
	failures += expect_uint("send 100B: snd_nxt = 1101", t.snd_nxt, 1101);
	failures += expect_uint("send 100B: snd_una unchanged", t.snd_una, 1001);

	recv_ack(&t, 1051);                 /* partial ack */
	failures += expect_uint("partial ack: snd_una = 1051", t.snd_una, 1051);

	recv_ack(&t, 1101);                 /* full ack */
	failures += expect_uint("full ack: snd_una = 1101", t.snd_una, 1101);

	/* Future ACK (peer broken or spoofed) is dropped. */
	recv_ack(&t, 9999);
	failures += expect_uint("future ack: snd_una unchanged", t.snd_una, 1101);

	/* Incoming data advances rcv_nxt. */
	recv_data(&t, 5001, 200);
	failures += expect_uint("recv 200B: rcv_nxt = 5201", t.rcv_nxt, 5201);

	/* Out-of-order seg dropped (this sim doesn't queue). */
	recv_data(&t, 6000, 50);
	failures += expect_uint("OOO seg: rcv_nxt unchanged", t.rcv_nxt, 5201);

	/* --- active close ---------------------------------------------------- */
	close_active(&t);
	failures += expect_int("close: state = FIN_WAIT_1", t.state, S_FIN_WAIT_1);
	failures += expect_uint("close: snd_nxt = 1102 (FIN consumes 1)",
	                        t.snd_nxt, 1102);

	recv_fin_ack(&t, 1102);
	failures += expect_int("fin acked: state = FIN_WAIT_2", t.state, S_FIN_WAIT_2);

	recv_peer_fin(&t, 5201);
	failures += expect_int("peer fin: state = TIME_WAIT",   t.state, S_TIME_WAIT);
	failures += expect_uint("peer fin: rcv_nxt += 1",       t.rcv_nxt, 5202);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
