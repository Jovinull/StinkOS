/* Host-side test for the ipv4_is_unicast helper in
 * kernel/drivers/net/net.h. Used by seven sites across the kernel to
 * decide whether a v4 address represents a valid routable peer (TCP
 * connect target, PING target, RX source filter, ICMP unreach dst,
 * IPv4 martian filter, etc). All those call sites collapse to this
 * one helper; if its decision flips even by one case, every guard
 * silently flips with it -- so the table is the single source of truth.
 *
 * Note: addresses are stored in network byte order, so on a little-
 * endian host the FIRST octet sits in the low 8 bits of the integer.
 * 192.168.1.1 = 0x0101A8C0, not 0xC0A80101.
 */
#include <stdio.h>

typedef unsigned int ipv4_t;

static int ipv4_is_unicast(ipv4_t addr)
{
	if (addr == 0 || addr == 0xFFFFFFFFu)
		return 0;
	if ((addr & 0x000000FFu) == 0x0000007Fu)
		return 0;
	if ((addr & 0x000000F0u) == 0x000000E0u)
		return 0;
	return 1;
}

static ipv4_t v4(unsigned char a, unsigned char b, unsigned char c, unsigned char d)
{
	return (ipv4_t)a |
	       ((ipv4_t)b <<  8) |
	       ((ipv4_t)c << 16) |
	       ((ipv4_t)d << 24);
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

	/* --- the four classes the helper specifically rejects ----------- */
	failures += expect("0.0.0.0 rejected",            ipv4_is_unicast(0),                       0);
	failures += expect("255.255.255.255 rejected",    ipv4_is_unicast(0xFFFFFFFFu),             0);
	failures += expect("127.0.0.1 rejected",          ipv4_is_unicast(v4(127, 0, 0, 1)),        0);
	failures += expect("127.1.2.3 rejected",          ipv4_is_unicast(v4(127, 1, 2, 3)),        0);
	failures += expect("127.255.255.254 rejected",    ipv4_is_unicast(v4(127, 255, 255, 254)),  0);
	failures += expect("224.0.0.1 (multicast lo)",    ipv4_is_unicast(v4(224, 0, 0, 1)),        0);
	failures += expect("224.0.0.251 (mDNS)",          ipv4_is_unicast(v4(224, 0, 0, 251)),      0);
	failures += expect("239.255.255.255 (mcast hi)",  ipv4_is_unicast(v4(239, 255, 255, 255)),  0);

	/* --- typical unicast addresses --------------------------------- */
	failures += expect("1.2.3.4 accepted",            ipv4_is_unicast(v4(1, 2, 3, 4)),          1);
	failures += expect("8.8.8.8 (Google DNS)",        ipv4_is_unicast(v4(8, 8, 8, 8)),          1);
	failures += expect("10.0.0.1 (RFC1918)",          ipv4_is_unicast(v4(10, 0, 0, 1)),         1);
	failures += expect("192.168.1.1 (RFC1918)",       ipv4_is_unicast(v4(192, 168, 1, 1)),      1);
	failures += expect("172.16.0.1 (RFC1918)",        ipv4_is_unicast(v4(172, 16, 0, 1)),       1);
	failures += expect("169.254.1.1 (link-local)",    ipv4_is_unicast(v4(169, 254, 1, 1)),      1);

	/* --- boundary: 126.255.255.255 just below 127/8 ---------------- */
	failures += expect("126.255.255.255 accepted",    ipv4_is_unicast(v4(126, 255, 255, 255)),  1);
	/* --- boundary: 128.0.0.0 just above 127/8 ---------------------- */
	failures += expect("128.0.0.0 accepted",          ipv4_is_unicast(v4(128, 0, 0, 0)),        1);

	/* --- boundary: 223.255.255.255 just below 224/4 ---------------- */
	failures += expect("223.255.255.255 accepted",    ipv4_is_unicast(v4(223, 255, 255, 255)),  1);
	/* --- boundary: 240.0.0.0 (class-E reserved, but NOT mcast, so
	 *      this helper accepts it -- a stricter classifier might not). */
	failures += expect("240.0.0.0 accepted",          ipv4_is_unicast(v4(240, 0, 0, 0)),        1);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
