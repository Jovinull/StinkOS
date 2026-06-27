/* Host-side test for the dup-ACK -> fast-retransmit decision in
 * kernel/drivers/net/tcp.c (the ESTABLISHED / CLOSE_WAIT ACK branch).
 *
 * Policy (RFC 5681 §3.2):
 *   - A "dup ACK" is an ACK that does not advance snd_una, carries no
 *     payload, and still has unacked data in flight.
 *   - Three dup ACKs in a row trigger fast retransmit and tighten
 *     ssthresh/cwnd to max(in_flight/2, 2*MSS).
 *   - Any forward ACK (acked > 0) resets the counter back to 0.
 *
 * This test replicates the counter + threshold + ssthresh formula so
 * that any change to either side surfaces as a test diff.
 */
#include <stdio.h>

#define TCP_MSS 536u

struct sim {
	unsigned char dup_acks;
	unsigned int  cwnd;
	unsigned int  ssthresh;
	unsigned int  retransmits;   /* total fast-retransmits triggered */
};

static void sim_reset(struct sim *s)
{
	s->dup_acks    = 0;
	s->cwnd        = 16u * TCP_MSS;
	s->ssthresh    = 64u * TCP_MSS;
	s->retransmits = 0;
}

/* Mirror of the ACK branch: given (acked, payload_len, inflight),
 * update the dup-ACK counter and trigger a fast retransmit on the 3rd
 * consecutive dup. */
static void sim_on_ack(struct sim *s,
                       unsigned int acked,
                       unsigned int payload,
                       unsigned int inflight)
{
	if (acked == 0 && payload == 0 && inflight > 0) {
		if (s->dup_acks < 255)
			s->dup_acks++;
		if (s->dup_acks == 3) {
			s->ssthresh = inflight / 2;
			if (s->ssthresh < 2u * TCP_MSS)
				s->ssthresh = 2u * TCP_MSS;
			s->cwnd = s->ssthresh;
			s->retransmits++;
		}
	} else if (acked > 0) {
		s->dup_acks = 0;
	}
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

	/* 1 dup ACK alone does NOT retransmit. */
	sim_reset(&s);
	sim_on_ack(&s, 0, 0, 10u * TCP_MSS);
	failures += expect_uint("1 dup: counter=1",      s.dup_acks,    1);
	failures += expect_uint("1 dup: no retransmit",  s.retransmits, 0);

	/* 2 dup ACKs still no retransmit (the threshold is 3). */
	sim_on_ack(&s, 0, 0, 10u * TCP_MSS);
	failures += expect_uint("2 dup: counter=2",      s.dup_acks,    2);
	failures += expect_uint("2 dup: no retransmit",  s.retransmits, 0);

	/* 3rd dup ACK triggers fast retransmit exactly once. */
	sim_on_ack(&s, 0, 0, 10u * TCP_MSS);
	failures += expect_uint("3 dup: counter=3",       s.dup_acks,    3);
	failures += expect_uint("3 dup: retransmits=1",   s.retransmits, 1);
	failures += expect_uint("3 dup: ssthresh halved", s.ssthresh,    5u * TCP_MSS);
	failures += expect_uint("3 dup: cwnd = ssthresh", s.cwnd,        5u * TCP_MSS);

	/* Subsequent dup ACKs do NOT re-trigger on every byte (counter keeps
	 * climbing past 3; only the exact "== 3" edge fires). NewReno
	 * inflation is intentionally not implemented yet. */
	sim_on_ack(&s, 0, 0, 10u * TCP_MSS);
	sim_on_ack(&s, 0, 0, 10u * TCP_MSS);
	failures += expect_uint("post-3 dups: still 1 retransmit", s.retransmits, 1);

	/* New ACK (acked > 0) clears the dup counter. */
	sim_on_ack(&s, TCP_MSS, 0, 9u * TCP_MSS);
	failures += expect_uint("forward ACK: counter cleared", s.dup_acks, 0);

	/* A second burst of dup ACKs after a forward ACK can retransmit again. */
	sim_on_ack(&s, 0, 0, 9u * TCP_MSS);
	sim_on_ack(&s, 0, 0, 9u * TCP_MSS);
	sim_on_ack(&s, 0, 0, 9u * TCP_MSS);
	failures += expect_uint("second burst: retransmits=2", s.retransmits, 2);

	/* Payload-bearing ACK is NOT a dup even with acked==0. */
	sim_reset(&s);
	sim_on_ack(&s, 0, 100, 10u * TCP_MSS);
	sim_on_ack(&s, 0, 100, 10u * TCP_MSS);
	sim_on_ack(&s, 0, 100, 10u * TCP_MSS);
	failures += expect_uint("payload ACKs do not dup", s.dup_acks,    0);
	failures += expect_uint("payload ACKs: no retx",   s.retransmits, 0);

	/* ssthresh floor: very small in_flight clamps to 2*MSS. */
	sim_reset(&s);
	sim_on_ack(&s, 0, 0, TCP_MSS);
	sim_on_ack(&s, 0, 0, TCP_MSS);
	sim_on_ack(&s, 0, 0, TCP_MSS);
	failures += expect_uint("small in_flight: ssthresh floor", s.ssthresh, 2u * TCP_MSS);
	failures += expect_uint("small in_flight: cwnd = floor",   s.cwnd,     2u * TCP_MSS);

	/* No retransmit when nothing in flight (inflight == 0 means the ACK
	 * is purely a window update / handshake artefact). */
	sim_reset(&s);
	sim_on_ack(&s, 0, 0, 0);
	sim_on_ack(&s, 0, 0, 0);
	sim_on_ack(&s, 0, 0, 0);
	failures += expect_uint("idle dup ACKs: counter=0",   s.dup_acks,    0);
	failures += expect_uint("idle dup ACKs: no retx",     s.retransmits, 0);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
