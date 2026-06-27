/* Host-side test for the TCP window-scale option parser in
 * kernel/drivers/net/tcp.c (tcp_parse_wscale). Mirrors RFC 7323:
 *   - kind=3, len=3 carries one shift byte (0..14)
 *   - kind=0 ends the option list
 *   - kind=1 is NOP padding (1-byte, no length)
 *   - any malformed length stops the scan and returns 0
 *   - shifts >14 are clamped to 14 (the RFC ceiling)
 *
 * A regression here silently breaks throughput on real-world peers
 * that negotiate large windows -- QEMU's default test peer doesn't
 * advertise scale, so the bug would never surface in CI.
 */
#include <stdio.h>

static unsigned char parse_wscale(const unsigned char *opts,
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

static int expect_u(const char *label, unsigned char got, unsigned char want)
{
	if (got == want) { printf("ok   %-55s = %u\n", label, got); return 0; }
	printf("FAIL %s: got %u, want %u\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;

	/* Empty options: 0. */
	failures += expect_u("empty options -> 0",
	                     parse_wscale((unsigned char *)"", 0), 0);

	/* Lone EOL: 0. */
	{
		unsigned char opts[] = { 0 };
		failures += expect_u("EOL only -> 0", parse_wscale(opts, 1), 0);
	}

	/* WScale at the head: shift=7. */
	{
		unsigned char opts[] = { 3, 3, 7 };
		failures += expect_u("[wscale 7] -> 7", parse_wscale(opts, 3), 7);
	}

	/* WScale after NOPs and MSS: shift=12. RFC layout often inserts
	 * a single NOP before kind=3 to align on a word boundary. */
	{
		unsigned char opts[] = {
			2, 4, 0x05, 0xB4,   /* MSS 1460 */
			1,                  /* NOP padding */
			3, 3, 12,           /* WScale 12 */
			0                   /* EOL */
		};
		failures += expect_u("[MSS, NOP, wscale 12, EOL] -> 12",
		                     parse_wscale(opts, sizeof(opts)), 12);
	}

	/* Cap at 14: a hostile peer sends shift=23, parser clamps. */
	{
		unsigned char opts[] = { 3, 3, 23 };
		failures += expect_u("wscale=23 -> 14 (cap)",
		                     parse_wscale(opts, 3), 14);
	}
	{
		unsigned char opts[] = { 3, 3, 14 };
		failures += expect_u("wscale=14 -> 14 (max in-bounds)",
		                     parse_wscale(opts, 3), 14);
	}
	{
		unsigned char opts[] = { 3, 3, 0xFF };
		failures += expect_u("wscale=255 -> 14 (cap)",
		                     parse_wscale(opts, 3), 14);
	}

	/* Multi-NOP padding before EOL: 0. */
	{
		unsigned char opts[] = { 1, 1, 1, 0 };
		failures += expect_u("NOPs + EOL -> 0",
		                     parse_wscale(opts, 4), 0);
	}

	/* Truncated wscale option (kind+len without payload): scan stops, 0. */
	{
		unsigned char opts[] = { 3, 3 };          /* missing shift byte */
		failures += expect_u("truncated wscale -> 0",
		                     parse_wscale(opts, 2), 0);
	}

	/* optlen too small (<2): treat as malformed, stop scan. */
	{
		unsigned char opts[] = { 5, 1, 0, 0, 0 };
		failures += expect_u("malformed len=1 -> 0",
		                     parse_wscale(opts, 5), 0);
	}

	/* optlen that overruns the buffer: stop, 0. */
	{
		unsigned char opts[] = { 5, 8, 0, 0 };
		failures += expect_u("len overruns buf -> 0",
		                     parse_wscale(opts, 4), 0);
	}

	/* Lone kind without length byte: stop. */
	{
		unsigned char opts[] = { 3 };
		failures += expect_u("lone kind byte -> 0",
		                     parse_wscale(opts, 1), 0);
	}

	/* Non-matching options skipped, then wscale found. */
	{
		unsigned char opts[] = {
			4, 2,               /* SACK_PERM (kind=4 len=2) */
			3, 3, 5,            /* WScale 5 */
			0
		};
		failures += expect_u("[SACK_PERM, wscale 5] -> 5",
		                     parse_wscale(opts, sizeof(opts)), 5);
	}

	/* Wrong optlen for kind=3 (not 3): skipped, scan continues. */
	{
		unsigned char opts[] = {
			3, 4, 9, 0,         /* malformed wscale (len=4) */
			3, 3, 2             /* real wscale */
		};
		failures += expect_u("malformed wscale skipped, real wscale -> 2",
		                     parse_wscale(opts, sizeof(opts)), 2);
	}

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
