/* Host-side test for the IPv4 source-routing option detector added to
 * ip_handle in kernel/drivers/net/ipv4.c. RFC 7126 says every modern
 * host MUST drop packets bearing LSRR (option kind 131) or SSRR (137)
 * because they let an attacker dictate the return path; the kernel
 * scans the option block right after the IPv4 header and drops the
 * datagram on the first match.
 *
 * This test replicates the option walker exactly (kind/length parser,
 * NOP-skip, EOL stop, malformed-bail) and verifies the decision table.
 */
#include <stdio.h>

/* Returns 1 if the option block contains a source-routing option that
 * the kernel would treat as a drop trigger. Mirrors the loop body inside
 * ip_handle so the table is the single source of truth. */
static int has_srcroute(const unsigned char *opts, unsigned int olen)
{
	for (unsigned int i = 0; i < olen; ) {
		unsigned char kind = opts[i];
		if (kind == 0) return 0;        /* end of options */
		if (kind == 1) { i++; continue; } /* NOP */
		if (i + 1 >= olen) return 0;
		unsigned char optlen = opts[i + 1];
		if (optlen < 2 || i + optlen > olen) return 0;
		if (kind == 131 || kind == 137)
			return 1;
		i += optlen;
	}
	return 0;
}

static int expect(const char *label, int got, int want)
{
	if (got == want) { printf("ok   %-50s = %d\n", label, got); return 0; }
	printf("FAIL %s: got %d, want %d\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;

	/* Empty options. */
	failures += expect("empty options: clean",        has_srcroute((const unsigned char *)"", 0), 0);

	/* EOL-only. */
	{
		unsigned char o[] = { 0, 0, 0, 0 };
		failures += expect("EOL early: clean",     has_srcroute(o, sizeof(o)), 0);
	}

	/* NOPs only. */
	{
		unsigned char o[] = { 1, 1, 1, 1 };
		failures += expect("NOPs only: clean",     has_srcroute(o, sizeof(o)), 0);
	}

	/* MSS-like option (kind=2): not source-routing. */
	{
		unsigned char o[] = { 2, 4, 0x05, 0xB4 };
		failures += expect("MSS option: clean",    has_srcroute(o, sizeof(o)), 0);
	}

	/* LSRR (kind=131) at the start: detected. */
	{
		unsigned char o[] = { 131, 11, 1, 1, 1, 1, 2, 2, 2, 2, 0 };
		failures += expect("LSRR at start: detected", has_srcroute(o, sizeof(o)), 1);
	}

	/* SSRR (kind=137) at the start: detected. */
	{
		unsigned char o[] = { 137, 7, 1, 2, 3, 4, 0 };
		failures += expect("SSRR at start: detected", has_srcroute(o, sizeof(o)), 1);
	}

	/* SSRR after a NOP + benign option: detected. */
	{
		unsigned char o[] = { 1, 2, 4, 0x05, 0xB4, 137, 7, 1, 2, 3, 4, 0 };
		failures += expect("SSRR after NOP + MSS: detected",
		                   has_srcroute(o, sizeof(o)), 1);
	}

	/* Truncated SSRR header (optlen says more bytes than buffer): bail
	 * without flagging. The kernel drops on bad parse, but this layer
	 * only reports "found a srcroute option" / "did not". */
	{
		unsigned char o[] = { 1, 137, 99 };       /* len=3 but optlen=99 */
		failures += expect("truncated SSRR: bail",
		                   has_srcroute(o, sizeof(o)), 0);
	}

	/* optlen = 0 / 1: malformed, parser stops. */
	{
		unsigned char o[] = { 137, 0 };
		failures += expect("optlen=0: bail",       has_srcroute(o, sizeof(o)), 0);
	}
	{
		unsigned char o[] = { 137, 1 };
		failures += expect("optlen=1: bail",       has_srcroute(o, sizeof(o)), 0);
	}

	/* SACK-like options should NOT be confused. */
	{
		unsigned char o[] = { 4, 2, 5, 10, 0,0,0,1, 0,0,0,5, 0 };
		failures += expect("SACK-like benign: clean",
		                   has_srcroute(o, sizeof(o)), 0);
	}

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
