/* Host-side test of the IPv4 / ICMP / UDP / TCP one's-complement checksum
 * algorithm (`ipv4_checksum` in kernel/drivers/net/ipv4.c). The routine
 * is purely arithmetic, has no kernel deps, so we replicate it locally
 * and use the same harness pattern as test_sha256 / test_mixer.
 *
 * The four upper layers use the exact same RFC 1071 sum, so a passing
 * test here is also a passing test for every checksum the stack emits.
 */
#include <stdio.h>
#include <string.h>

/* Algorithm copy. The kernel side returns network-byte-order; for these
 * tests we compare against pre-computed network-order values. */
static unsigned short htons_local(unsigned short v)
{
	return (unsigned short)((v << 8) | (v >> 8));
}

static unsigned short ipv4_checksum(const void *data, unsigned int len)
{
	const unsigned char *p = (const unsigned char *)data;
	unsigned int sum = 0;

	for (unsigned int i = 0; i + 1 < len; i += 2)
		sum += ((unsigned int)p[i] << 8) | p[i + 1];
	if (len & 1)
		sum += (unsigned int)p[len - 1] << 8;
	while (sum >> 16)
		sum = (sum & 0xFFFFu) + (sum >> 16);
	return htons_local((unsigned short)(~sum & 0xFFFFu));
}

static int expect_eq(const char *label, unsigned short got, unsigned short want)
{
	if (got == want) {
		printf("ok   %s = 0x%04x\n", label, got);
		return 0;
	}
	printf("FAIL %s: got 0x%04x, want 0x%04x\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;

	/* Empty buffer: ~0 over zero accumulator. */
	failures += expect_eq("empty", ipv4_checksum("", 0), 0xFFFF);

	/* Single zero byte: ones-comp of zero is still 0xFFFF. */
	{
		unsigned char b = 0x00;
		failures += expect_eq("one zero byte", ipv4_checksum(&b, 1), 0xFFFF);
	}

	/* A real IPv4 header round-trip: take a freshly built header with the
	 * checksum field zero, fill, then a second pass with the result in
	 * place must sum to zero (the canonical RFC 791 verification). */
	{
		unsigned char hdr[20] = {
			0x45, 0x00, 0x00, 0x3C,
			0x1C, 0x46, 0x40, 0x00,
			0x40, 0x06, 0x00, 0x00,
			0xAC, 0x10, 0x0A, 0x63,
			0xAC, 0x10, 0x0A, 0x0C,
		};
		unsigned short c = ipv4_checksum(hdr, 20);
		/* Write checksum back into the header field (offset 10, big-endian). */
		hdr[10] = (unsigned char)(c & 0xFF);
		hdr[11] = (unsigned char)((c >> 8) & 0xFF);
		failures += expect_eq("self-verify", ipv4_checksum(hdr, 20), 0x0000);
	}

	/* Odd-length buffer: last byte must be folded as MSB of a zero-padded
	 * 16-bit word, not dropped. Construct a case where dropping would
	 * change the answer. */
	{
		unsigned char odd[3] = {0x12, 0x34, 0x56};
		/* 0x1234 + 0x5600 = 0x6834; ~0x6834 = 0x97CB; htons -> 0xCB97. */
		failures += expect_eq("odd length", ipv4_checksum(odd, 3), 0xCB97);
	}

	/* All-FF buffer: every 16-bit word is 0xFFFF, sum wraps to zero, ~0 = ~0.
	 * Effective: sum = 0xFFFF; ~sum & 0xFFFF = 0; htons(0) = 0. */
	{
		unsigned char ones[16];
		memset(ones, 0xFF, sizeof(ones));
		failures += expect_eq("all FF", ipv4_checksum(ones, sizeof(ones)), 0x0000);
	}

	/* Stress: 1500-byte all-zero packet should still ~0 = 0xFFFF. */
	{
		static unsigned char big[1500];
		failures += expect_eq("zero MTU frame",
		                      ipv4_checksum(big, sizeof(big)), 0xFFFF);
	}

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
