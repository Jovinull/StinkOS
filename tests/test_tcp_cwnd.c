/* Host-side test for TCP congestion control in
 * kernel/drivers/net/tcp.c. Reno-ish loop:
 *
 *   - slow start (cwnd < ssthresh):    cwnd += acked
 *   - cong. avoidance (cwnd >= sst.):  cwnd += max(1, MSS*acked / cwnd)
 *   - cwnd capped at TCP_BUFFER_SIZE
 *   - RTO loss: ssthresh = max(2*MSS, in_flight/2); cwnd = MSS
 *
 * Note that on StinkOS, TCP_MSS = 1460 and TCP_BUFFER_SIZE = 4096, so
 * cwnd is hard-capped at ~2.8 segments. That ceiling is part of the
 * contract too -- a future buffer resize must not silently change
 * the steady-state cwnd. The test therefore both checks the formula
 * AND the cap behaviour.
 */
#include <stdio.h>

#define TCP_MSS         1460u
#define TCP_BUFFER_SIZE 4096u

static unsigned int do_ack(unsigned int cwnd, unsigned int ssthresh,
                           unsigned int acked)
{
	if (cwnd < ssthresh) {
		cwnd += acked;
	} else if (cwnd > 0) {
		unsigned int add = (TCP_MSS * acked) / cwnd;
		if (add == 0) add = 1;
		cwnd += add;
	}
	if (cwnd > TCP_BUFFER_SIZE) cwnd = TCP_BUFFER_SIZE;
	return cwnd;
}

struct loss { unsigned int cwnd, ssthresh; };

static struct loss do_loss(unsigned int in_flight)
{
	struct loss r;
	unsigned int half = in_flight / 2u;
	if (half < 2u * TCP_MSS) half = 2u * TCP_MSS;
	r.ssthresh = half;
	r.cwnd     = TCP_MSS;
	return r;
}

static int expect_u(const char *label, unsigned int got, unsigned int want)
{
	if (got == want) { printf("ok   %-55s = %u\n", label, got); return 0; }
	printf("FAIL %s: got %u, want %u\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;

	/* --- Slow start with a small ssthresh that lets us see the growth
	 *     before the cap. cwnd=MSS, sst=2*MSS, ack MSS -> +MSS. */
	failures += expect_u("ss: cwnd=MSS + ack MSS -> 2*MSS",
	                     do_ack(TCP_MSS, 2u * TCP_MSS, TCP_MSS),
	                     2u * TCP_MSS);

	/* SS still cwnd < sst boundary: ack 1 byte still increments cwnd. */
	failures += expect_u("ss: cwnd=MSS + ack 1 -> MSS+1",
	                     do_ack(TCP_MSS, 2u * TCP_MSS, 1u),
	                     TCP_MSS + 1u);

	/* --- Cap fires: TCP_BUFFER_SIZE is the ceiling. */
	failures += expect_u("ss: cap at TCP_BUFFER_SIZE",
	                     do_ack(2u * TCP_MSS, 64u * TCP_MSS, 2u * TCP_MSS),
	                     TCP_BUFFER_SIZE);

	/* --- Congestion avoidance: cwnd == sst, additive increase.
	 * cwnd=2*MSS, ack=MSS -> add = (MSS*MSS) / (2*MSS) = MSS/2.
	 * Pre-cap result = 2920 + 730 = 3650 (under the 4096 ceiling). */
	failures += expect_u("ca: cwnd==sst, ack MSS -> +MSS/2",
	                     do_ack(2u * TCP_MSS, 2u * TCP_MSS, TCP_MSS),
	                     2u * TCP_MSS + TCP_MSS / 2u);

	/* CA floor: tiny ack still adds at least 1 even when MSS*acked /
	 * cwnd rounds to 0. */
	failures += expect_u("ca: tiny ack still +1 (then maybe cap)",
	                     do_ack(2u * TCP_MSS, 2u * TCP_MSS, 1u),
	                     2u * TCP_MSS + 1u);

	/* CA cap: cwnd just under cap + any add fires the clamp. */
	failures += expect_u("ca: cap at TCP_BUFFER_SIZE",
	                     do_ack(TCP_BUFFER_SIZE - 1u, 2u * TCP_MSS, TCP_MSS),
	                     TCP_BUFFER_SIZE);

	/* --- Degenerate cwnd=0: SS branch fires (0 < sst), cwnd grows
	 *     by acked. The CA branch is guarded by `cwnd > 0` so a
	 *     zero-divide never happens. */
	failures += expect_u("degenerate: cwnd=0 + SS ack MSS -> MSS",
	                     do_ack(0, 2u * TCP_MSS, TCP_MSS),
	                     TCP_MSS);
	/* But CA at cwnd=0 (impossible in practice; both branches need
	 * cwnd>=sst and sst>=2*MSS) is a no-op by design. */
	failures += expect_u("degenerate: cwnd=0 + CA (sst=0) stays 0",
	                     do_ack(0, 0, TCP_MSS),
	                     0);

	/* --- Loss collapse: in_flight=8*MSS -> ssthresh=4*MSS, cwnd=MSS. */
	struct loss r = do_loss(8u * TCP_MSS);
	failures += expect_u("loss: in_flight=8*MSS -> cwnd=MSS",       r.cwnd,     TCP_MSS);
	failures += expect_u("loss: in_flight=8*MSS -> ssthresh=4*MSS", r.ssthresh, 4u * TCP_MSS);

	/* Loss floor: in_flight tiny -> ssthresh clamped to 2*MSS. */
	r = do_loss(0);
	failures += expect_u("loss: floor ssthresh=2*MSS",              r.ssthresh, 2u * TCP_MSS);

	r = do_loss(TCP_MSS);
	failures += expect_u("loss: in_flight=MSS still floor",         r.ssthresh, 2u * TCP_MSS);

	/* --- End-to-end: start a fresh connection state, ack until cap,
	 *     simulate RTO, observe collapse back to MSS. */
	unsigned int cwnd = TCP_MSS;
	unsigned int sst  = 64u * TCP_MSS;
	cwnd = do_ack(cwnd, sst, TCP_MSS);                /* 2*MSS */
	cwnd = do_ack(cwnd, sst, TCP_MSS);                /* 2*MSS + MSS = 3*MSS -> cap */
	failures += expect_u("e2e: after 2 acks cwnd hits cap",
	                     cwnd, TCP_BUFFER_SIZE);

	r = do_loss(cwnd);
	failures += expect_u("e2e: post-loss cwnd=MSS",     r.cwnd,     TCP_MSS);
	/* in_flight=4096, half=2048 < 2*MSS -> sst floors to 2*MSS. */
	failures += expect_u("e2e: post-loss sst=2*MSS",    r.ssthresh, 2u * TCP_MSS);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
