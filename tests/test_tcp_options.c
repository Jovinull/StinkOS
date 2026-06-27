/* Host-side test for the TCP options parser that lives in tcp.c
 * (`tcp_parse_wscale`). The kernel version is internally `static`, so we
 * replicate it here and cover the cases that historically tripped naive
 * implementations: EOL early-exit, NOP padding, malformed length, kind
 * ordering, and the RFC 7323 shift cap.
 */
#include <stdio.h>
#include <string.h>

/* Mirror of tcp_parse_wscale from kernel/drivers/net/tcp.c. Any change
 * to that function should land here too, with a matching test case. */
static unsigned char tcp_parse_wscale(const unsigned char *opts,
                                      unsigned int opts_len)
{
	for (unsigned int i = 0; i < opts_len; ) {
		unsigned char kind = opts[i];
		if (kind == 0) break;
		if (kind == 1) { i++; continue; }
		if (i + 1 >= opts_len) break;
		unsigned char optlen = opts[i + 1];
		if (optlen < 2 || i + optlen > opts_len) break;
		if (kind == 3 && optlen == 3) {
			unsigned char shift = opts[i + 2];
			if (shift > 14) shift = 14;
			return shift;
		}
		i += optlen;
	}
	return 0;
}

static int expect_eq(const char *label, unsigned char got, unsigned char want)
{
	if (got == want) {
		printf("ok   %s = %u\n", label, got);
		return 0;
	}
	printf("FAIL %s: got %u, want %u\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;

	/* Empty options buffer. */
	failures += expect_eq("empty", tcp_parse_wscale((unsigned char *)"", 0), 0);

	/* Only NOPs. */
	{
		unsigned char opts[] = {1, 1, 1};
		failures += expect_eq("only NOPs",
		                      tcp_parse_wscale(opts, sizeof(opts)), 0);
	}

	/* Bare window-scale option, shift = 7. */
	{
		unsigned char opts[] = {3, 3, 7};
		failures += expect_eq("bare wscale 7",
		                      tcp_parse_wscale(opts, sizeof(opts)), 7);
	}

	/* NOP NOP wscale -- the alignment-friendly form Linux emits. */
	{
		unsigned char opts[] = {1, 1, 3, 3, 5};
		failures += expect_eq("padded wscale 5",
		                      tcp_parse_wscale(opts, sizeof(opts)), 5);
	}

	/* MSS option (kind=2, len=4) before wscale -- parser must skip MSS. */
	{
		unsigned char opts[] = {2, 4, 0x05, 0xB4, 3, 3, 3};
		failures += expect_eq("MSS then wscale",
		                      tcp_parse_wscale(opts, sizeof(opts)), 3);
	}

	/* RFC 7323 shift cap: anything >14 is clamped. */
	{
		unsigned char opts[] = {3, 3, 30};
		failures += expect_eq("shift clamped",
		                      tcp_parse_wscale(opts, sizeof(opts)), 14);
	}

	/* EOL kind=0 cuts off scanning -- a wscale after EOL is invisible. */
	{
		unsigned char opts[] = {0, 3, 3, 9};
		failures += expect_eq("EOL aborts scan",
		                      tcp_parse_wscale(opts, sizeof(opts)), 0);
	}

	/* Malformed: optlen < 2 must abort cleanly, not loop forever. */
	{
		unsigned char opts[] = {2, 0, 3, 3, 4};
		failures += expect_eq("bad optlen 0",
		                      tcp_parse_wscale(opts, sizeof(opts)), 0);
	}

	/* Malformed: optlen extends past buffer. */
	{
		unsigned char opts[] = {2, 8, 0};
		failures += expect_eq("optlen overruns",
		                      tcp_parse_wscale(opts, sizeof(opts)), 0);
	}

	/* Wrong length on kind=3 (must be exactly 3). */
	{
		unsigned char opts[] = {3, 4, 7, 0};
		failures += expect_eq("wscale wrong len",
		                      tcp_parse_wscale(opts, sizeof(opts)), 0);
	}

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
