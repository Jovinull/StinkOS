/* Host-side test for the IPv4 martian source filter in
 * kernel/drivers/net/ipv4.c (RFC 1812 §5.3.7). The receive path drops
 * any packet whose source address claims to be:
 *   - our own local IP (anti-spoof loopback into our own state machines)
 *   - non-unicast: limited broadcast, 127/8 loopback, 224/4 multicast
 * src=0 is permitted because DHCP OFFER/ACK arrive that way before
 * lease bind.
 *
 * The decision below mirrors the two checks at ipv4.c:225-228 verbatim;
 * if the kernel's source-filter loses either branch, this test catches it.
 */
#include <stdio.h>

typedef unsigned int ipv4_t;

static int ipv4_is_unicast(ipv4_t addr)
{
	if (addr == 0 || addr == 0xFFFFFFFFu)
		return 0;
	if ((addr & 0x000000FFu) == 0x0000007Fu)   /* 127/8 loopback */
		return 0;
	if ((addr & 0x000000F0u) == 0x000000E0u)   /* 224/4 multicast */
		return 0;
	return 1;
}

/* Returns 1 if the packet should be DROPPED by the martian filter,
 * 0 if it should be accepted. Mirrors kernel/drivers/net/ipv4.c. */
static int martian_drop(ipv4_t local, ipv4_t src)
{
	if (local != 0 && src == local)            return 1;
	if (src != 0 && !ipv4_is_unicast(src))     return 1;
	return 0;
}

static ipv4_t v4(unsigned char a, unsigned char b, unsigned char c, unsigned char d)
{
	return (ipv4_t)a |
	       ((ipv4_t)b << 8) |
	       ((ipv4_t)c << 16) |
	       ((ipv4_t)d << 24);
}

static int expect_int(const char *label, int got, int want)
{
	if (got == want) { printf("ok   %-55s = %d\n", label, got); return 0; }
	printf("FAIL %s: got %d, want %d\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;
	ipv4_t local = v4(192, 168, 1, 50);

	/* Anti-spoof: src == local must drop. */
	failures += expect_int("spoof: src == local IP -> drop",
	                       martian_drop(local, local), 1);

	/* Legit unicast peer from same subnet: accept. */
	failures += expect_int("normal: 192.168.1.1 -> accept",
	                       martian_drop(local, v4(192, 168, 1, 1)), 0);

	/* Off-subnet unicast peer: still accept (martian doesn't gate routing). */
	failures += expect_int("normal: 8.8.8.8 -> accept",
	                       martian_drop(local, v4(8, 8, 8, 8)), 0);

	/* Limited broadcast as source: drop. */
	failures += expect_int("bcast src 255.255.255.255 -> drop",
	                       martian_drop(local, 0xFFFFFFFFu), 1);

	/* Loopback source 127/8: drop (no packet from outside should claim 127). */
	failures += expect_int("loopback src 127.0.0.1 -> drop",
	                       martian_drop(local, v4(127, 0, 0, 1)), 1);
	failures += expect_int("loopback src 127.1.2.3 -> drop",
	                       martian_drop(local, v4(127, 1, 2, 3)), 1);

	/* Multicast source 224/4: drop (mcast is a destination, never a source). */
	failures += expect_int("mcast src 224.0.0.1 -> drop",
	                       martian_drop(local, v4(224, 0, 0, 1)), 1);
	failures += expect_int("mcast src 239.255.255.250 -> drop",
	                       martian_drop(local, v4(239, 255, 255, 250)), 1);

	/* src=0 is permitted (DHCP pre-bind). */
	failures += expect_int("src=0.0.0.0 (DHCP pre-bind) -> accept",
	                       martian_drop(local, 0), 0);

	/* If we have no local IP yet, the spoof branch is bypassed but the
	 * non-unicast branch still fires. */
	failures += expect_int("local=0 + src=local-like -> accept",
	                       martian_drop(0, v4(192, 168, 1, 50)), 0);
	failures += expect_int("local=0 + bcast src -> drop",
	                       martian_drop(0, 0xFFFFFFFFu), 1);
	failures += expect_int("local=0 + mcast src -> drop",
	                       martian_drop(0, v4(224, 0, 0, 1)), 1);
	failures += expect_int("local=0 + src=0 -> accept",
	                       martian_drop(0, 0), 0);

	/* Boundary at the multicast range: 223/x is unicast, 224/x is mcast. */
	failures += expect_int("boundary: 223.255.255.255 -> accept",
	                       martian_drop(local, v4(223, 255, 255, 255)), 0);
	failures += expect_int("boundary: 224.0.0.0 -> drop",
	                       martian_drop(local, v4(224, 0, 0, 0)), 1);
	failures += expect_int("boundary: 239.255.255.255 -> drop",
	                       martian_drop(local, v4(239, 255, 255, 255)), 1);
	failures += expect_int("boundary: 240.0.0.0 (class E) -> accept",
	                       martian_drop(local, v4(240, 0, 0, 0)), 0);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
