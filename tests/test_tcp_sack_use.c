/* Host-side test for sender-side SACK use in kernel/drivers/net/tcp.c.
 *
 * Two pieces:
 *   1. tcp_parse_sack_lo: pulls the LOWEST left edge from kind=5 SACK
 *      blocks in a raw TCP options buffer. Each block is 8 bytes
 *      (left, right) in network order; the option header is kind=5,
 *      length = 2 + 8*N for N blocks. Returns 0 when absent / malformed.
 *
 *   2. retransmit chunk clamp: when peer_sack_lo > snd_una, clamp the
 *      next retransmit length so we never resend bytes that lie inside a
 *      SACKed range. The classic case is "snd_una=100, peer ACKs 100
 *      with SACK=(200, 1000); a full MSS retransmit would re-send
 *      [100, 636] but the peer already has [200, 636] -- so the
 *      retransmit must shrink to [100, 200] (100 bytes).
 *
 * The replicas below mirror the kernel logic byte for byte.
 */
#include <stdio.h>

#define TCP_MSS 536u

/* ---- replica of tcp_parse_sack_lo ----------------------------------- */

static unsigned int parse_sack_lo(const unsigned char *opts,
                                  unsigned int opts_len)
{
	unsigned int lowest = 0;
	int seen = 0;
	for (unsigned int i = 0; i < opts_len; ) {
		unsigned char kind = opts[i];
		if (kind == 0) break;
		if (kind == 1) { i++; continue; }
		if (i + 1 >= opts_len) break;
		unsigned char optlen = opts[i + 1];
		if (optlen < 2 || i + optlen > opts_len) break;
		if (kind == 5 && optlen >= 10 && ((optlen - 2u) % 8u) == 0) {
			unsigned int n = (optlen - 2u) / 8u;
			for (unsigned int b = 0; b < n; b++) {
				unsigned int off = i + 2u + b * 8u;
				unsigned int left =
				    ((unsigned int)opts[off + 0] << 24) |
				    ((unsigned int)opts[off + 1] << 16) |
				    ((unsigned int)opts[off + 2] <<  8) |
				    ((unsigned int)opts[off + 3]);
				if (!seen || left < lowest) {
					lowest = left;
					seen = 1;
				}
			}
		}
		i += optlen;
	}
	return seen ? lowest : 0u;
}

/* ---- replica of the retransmit chunk clamp ------------------------- */

static unsigned int clamp_chunk(unsigned int snd_una,
                                unsigned int snd_nxt,
                                unsigned int peer_sack_lo)
{
	unsigned int chunk = snd_nxt - snd_una;
	if (chunk > TCP_MSS) chunk = TCP_MSS;
	if (peer_sack_lo != 0) {
		unsigned int gap = peer_sack_lo - snd_una;
		if (gap > 0 && chunk > gap) chunk = gap;
	}
	return chunk;
}

/* ---- helpers ------------------------------------------------------- */

static unsigned int put_sack(unsigned char *opts,
                             const unsigned int *blocks,
                             unsigned int n)
{
	opts[0] = 1; opts[1] = 1;             /* two NOPs to align SACK to 4 */
	opts[2] = 5;                          /* kind */
	opts[3] = (unsigned char)(2 + 8 * n); /* length */
	for (unsigned int b = 0; b < n; b++) {
		unsigned int left  = blocks[b * 2 + 0];
		unsigned int right = blocks[b * 2 + 1];
		opts[4 + b * 8 + 0] = (unsigned char)(left  >> 24);
		opts[4 + b * 8 + 1] = (unsigned char)(left  >> 16);
		opts[4 + b * 8 + 2] = (unsigned char)(left  >>  8);
		opts[4 + b * 8 + 3] = (unsigned char)(left       );
		opts[4 + b * 8 + 4] = (unsigned char)(right >> 24);
		opts[4 + b * 8 + 5] = (unsigned char)(right >> 16);
		opts[4 + b * 8 + 6] = (unsigned char)(right >>  8);
		opts[4 + b * 8 + 7] = (unsigned char)(right       );
	}
	return 4 + 8 * n;
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
	unsigned char opts[64] = {0};
	unsigned int n;

	/* Empty option set: no SACK present, lowest = 0. */
	failures += expect_uint("empty opts: no SACK", parse_sack_lo(opts, 0), 0u);

	/* MSS option only (no SACK): lowest = 0. */
	opts[0] = 2; opts[1] = 4; opts[2] = 0x05; opts[3] = 0xB4;   /* MSS=1460 */
	failures += expect_uint("MSS only: no SACK", parse_sack_lo(opts, 4), 0u);

	/* Single SACK block (200, 1000): lowest = 200. */
	{
		unsigned int blk[] = { 200u, 1000u };
		n = put_sack(opts, blk, 1);
		failures += expect_uint("1 block: lowest = 200",
		                        parse_sack_lo(opts, n), 200u);
	}

	/* Two SACK blocks reported out of order (1500, 2000) then (500, 800):
	 * lowest must come from the second block. */
	{
		unsigned int blk[] = { 1500u, 2000u, 500u, 800u };
		n = put_sack(opts, blk, 2);
		failures += expect_uint("2 blocks: lowest = 500 (across blocks)",
		                        parse_sack_lo(opts, n), 500u);
	}

	/* Malformed: SACK header length 9 (not 2 + 8*N) -- rejected. */
	opts[0] = 1; opts[1] = 1; opts[2] = 5; opts[3] = 9;
	for (int i = 0; i < 7; i++) opts[4 + i] = 0;
	failures += expect_uint("bad SACK length: ignored",
	                        parse_sack_lo(opts, 11), 0u);

	/* Truncated buffer cuts off the SACK option early. */
	{
		unsigned int blk[] = { 100u, 200u };
		n = put_sack(opts, blk, 1);
		failures += expect_uint("truncated opts: ignored",
		                        parse_sack_lo(opts, n - 2), 0u);
	}

	/* ---- clamp_chunk -------------------------------------------------- */

	/* No SACK: chunk = min(in_flight, MSS) -- classic Reno. */
	failures += expect_uint("no SACK: full MSS retransmit",
	                        clamp_chunk(100, 100 + 1000, 0), TCP_MSS);
	failures += expect_uint("no SACK, in_flight < MSS: chunk = in_flight",
	                        clamp_chunk(100, 100 + 200, 0), 200u);

	/* SACK gap larger than MSS: chunk still MSS. */
	failures += expect_uint("SACK gap > MSS: chunk = MSS",
	                        clamp_chunk(100, 100 + 1000, 100 + 700), TCP_MSS);

	/* SACK gap smaller than MSS: chunk clamped to gap. */
	failures += expect_uint("SACK gap = 100: chunk = 100",
	                        clamp_chunk(100, 100 + 1000, 200), 100u);

	/* SACK exactly at snd_una (gap=0): clamp is a no-op (kernel only stores
	 * peer_sack_lo when it lies strictly above snd_una; this test mimics
	 * the boundary by passing 0 = "no useful SACK"). */
	failures += expect_uint("SACK at snd_una: clamp inert",
	                        clamp_chunk(100, 100 + 1000, 0), TCP_MSS);

	/* SACK gap matches MSS exactly: chunk unchanged. */
	failures += expect_uint("SACK gap = MSS: chunk = MSS",
	                        clamp_chunk(100, 100 + 1000, 100 + TCP_MSS), TCP_MSS);

	/* SACK 1-byte ahead of snd_una: chunk shrinks to 1. */
	failures += expect_uint("SACK gap = 1: chunk = 1",
	                        clamp_chunk(100, 100 + 1000, 101), 1u);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
