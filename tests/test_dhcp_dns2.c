/* Host-side test for the secondary DNS server parse in
 * kernel/drivers/net/dhcp.c. RFC 2132 §3.8 says option 6 is a list of
 * 4-byte IPv4 server addresses in priority order. We keep up to two
 * (primary + secondary) -- enough that a server outage swaps over to
 * the fallback after half the DNS retry budget without resolving.
 *
 * Cases:
 *   - olen < 4: ignore (malformed)
 *   - olen == 4: primary set, secondary stays 0
 *   - olen == 8: both primary + secondary set
 *   - olen >= 12: third+ servers ignored (we only have two slots)
 *   - olen not multiple of 4 but >= 8: still take the first two, the
 *     surplus is undefined per RFC but we don't trip over it
 */
#include <stdio.h>

static unsigned int read_ipv4(const unsigned char *p)
{
	return  (unsigned int)p[0]        |
	       ((unsigned int)p[1] <<  8) |
	       ((unsigned int)p[2] << 16) |
	       ((unsigned int)p[3] << 24);
}

static unsigned int dns_ip, dns_ip2;

static void parse_dns_option(const unsigned char *opts, unsigned int olen)
{
	dns_ip = 0; dns_ip2 = 0;
	if (olen >= 4) dns_ip  = read_ipv4(&opts[0]);
	if (olen >= 8) dns_ip2 = read_ipv4(&opts[4]);
}

static int expect_uint(const char *label, unsigned int got, unsigned int want)
{
	if (got == want) { printf("ok   %-50s = 0x%08X\n", label, got); return 0; }
	printf("FAIL %s: got 0x%08X, want 0x%08X\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;
	unsigned char opts[16];

	/* Empty / too-short option: both stay 0. */
	parse_dns_option(opts, 0);
	failures += expect_uint("olen=0: primary stays 0",        dns_ip,  0);
	failures += expect_uint("olen=0: secondary stays 0",      dns_ip2, 0);

	parse_dns_option(opts, 3);
	failures += expect_uint("olen=3 (malformed): primary 0",  dns_ip,  0);

	/* Single primary: 8.8.8.8 (net order = 0x08080808). */
	opts[0]=8; opts[1]=8; opts[2]=8; opts[3]=8;
	parse_dns_option(opts, 4);
	failures += expect_uint("8.8.8.8 primary",                dns_ip,  0x08080808u);
	failures += expect_uint("4-byte option: no secondary",    dns_ip2, 0);

	/* Primary + secondary: 8.8.8.8 + 1.1.1.1. */
	opts[4]=1; opts[5]=1; opts[6]=1; opts[7]=1;
	parse_dns_option(opts, 8);
	failures += expect_uint("8.8.8.8 still primary",          dns_ip,  0x08080808u);
	failures += expect_uint("1.1.1.1 secondary",              dns_ip2, 0x01010101u);

	/* Three servers offered: third ignored (we have no third slot). */
	opts[8]=9; opts[9]=9; opts[10]=9; opts[11]=9;
	parse_dns_option(opts, 12);
	failures += expect_uint("3-server list: primary kept",    dns_ip,  0x08080808u);
	failures += expect_uint("3-server list: secondary kept",  dns_ip2, 0x01010101u);
	/* No way to assert "third dropped" except by not having a slot. */

	/* Odd length 7 (not multiple of 4): trailing byte ignored, first
	 * 4-byte primary OK, no secondary. */
	parse_dns_option(opts, 7);
	failures += expect_uint("olen=7: primary OK",             dns_ip,  0x08080808u);
	failures += expect_uint("olen=7: no secondary",           dns_ip2, 0);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
