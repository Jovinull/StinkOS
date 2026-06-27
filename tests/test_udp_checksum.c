/* Host-side test for the UDP checksum verifier added to
 * kernel/drivers/net/udp.c (udp_checksum_ok). RFC 768 leaves the UDP
 * checksum optional on IPv4, but if the sender does fill it in we MUST
 * verify -- otherwise a bit-flipped DHCP / DNS reply silently corrupts
 * the state machine.
 *
 * Rules:
 *   1. Field == 0 -> always accepted (sender opted out).
 *   2. Field != 0 -> include in the one's-complement sum across the
 *      pseudo-header + the whole UDP packet (header bytes 0..7 plus
 *      payload). A valid packet folds the sum to 0xFFFF.
 *   3. Any other sum -> drop.
 */
#include <stdio.h>
#include <string.h>

static int udp_checksum_ok(unsigned int src, unsigned int dst,
                           const unsigned char *pkt, unsigned int total)
{
	if (((unsigned short)pkt[6] << 8 | pkt[7]) == 0)
		return 1;

	unsigned int sum = 0;
	unsigned char ph[12];
	ph[0] = (unsigned char)(src      & 0xFF);
	ph[1] = (unsigned char)((src >>  8) & 0xFF);
	ph[2] = (unsigned char)((src >> 16) & 0xFF);
	ph[3] = (unsigned char)((src >> 24) & 0xFF);
	ph[4] = (unsigned char)(dst      & 0xFF);
	ph[5] = (unsigned char)((dst >>  8) & 0xFF);
	ph[6] = (unsigned char)((dst >> 16) & 0xFF);
	ph[7] = (unsigned char)((dst >> 24) & 0xFF);
	ph[8] = 0;
	ph[9] = 17;
	ph[10] = (unsigned char)((total >> 8) & 0xFF);
	ph[11] = (unsigned char)( total       & 0xFF);

	for (unsigned int i = 0; i + 1 < sizeof(ph); i += 2)
		sum += ((unsigned int)ph[i] << 8) | ph[i + 1];

	for (unsigned int i = 0; i + 1 < total; i += 2)
		sum += ((unsigned int)pkt[i] << 8) | pkt[i + 1];
	if (total & 1)
		sum += (unsigned int)pkt[total - 1] << 8;

	while (sum >> 16)
		sum = (sum & 0xFFFFu) + (sum >> 16);
	return (sum & 0xFFFFu) == 0xFFFFu;
}

/* Compute the one's-complement UDP checksum the sender SHOULD have put in
 * the checksum field so a verifier returns 1. Mirrors the same algorithm
 * but writes the result back -- used by the test to forge valid packets. */
static unsigned short udp_checksum(unsigned int src, unsigned int dst,
                                   const unsigned char *pkt, unsigned int total)
{
	unsigned int sum = 0;
	unsigned char ph[12];
	ph[0] = (unsigned char)(src      & 0xFF);
	ph[1] = (unsigned char)((src >>  8) & 0xFF);
	ph[2] = (unsigned char)((src >> 16) & 0xFF);
	ph[3] = (unsigned char)((src >> 24) & 0xFF);
	ph[4] = (unsigned char)(dst      & 0xFF);
	ph[5] = (unsigned char)((dst >>  8) & 0xFF);
	ph[6] = (unsigned char)((dst >> 16) & 0xFF);
	ph[7] = (unsigned char)((dst >> 24) & 0xFF);
	ph[8] = 0;
	ph[9] = 17;
	ph[10] = (unsigned char)((total >> 8) & 0xFF);
	ph[11] = (unsigned char)( total       & 0xFF);

	for (unsigned int i = 0; i + 1 < sizeof(ph); i += 2)
		sum += ((unsigned int)ph[i] << 8) | ph[i + 1];

	for (unsigned int i = 0; i + 1 < total; i += 2) {
		if (i == 6) continue;        /* skip the checksum field */
		sum += ((unsigned int)pkt[i] << 8) | pkt[i + 1];
	}
	if (total & 1)
		sum += (unsigned int)pkt[total - 1] << 8;

	while (sum >> 16)
		sum = (sum & 0xFFFFu) + (sum >> 16);
	return (unsigned short)(~sum & 0xFFFFu);
}

static int expect_int(const char *label, int got, int want)
{
	if (got == want) { printf("ok   %-45s = %d\n", label, got); return 0; }
	printf("FAIL %s: got %d, want %d\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;

	/* Sender opted out (checksum = 0): always accepted regardless of body. */
	{
		unsigned char p[16] = { 0x12,0x34, 0x56,0x78, 0,16, 0,0, 'h','e','l','l','o','!','!','!' };
		failures += expect_int("zero checksum: accepted",
		                       udp_checksum_ok(0x01020304u, 0x05060708u, p, sizeof(p)), 1);
	}

	/* Valid packet: compute the right checksum, then verify. */
	{
		unsigned char p[16] = { 0xAB,0xCD, 0x12,0x34, 0,16, 0,0, 'd','a','t','a','p','a','y','d' };
		unsigned short cs = udp_checksum(0x0A0B0C0Du, 0x0E0F1011u, p, sizeof(p));
		if (cs == 0) cs = 0xFFFF;       /* sender substitutes 0xFFFF for "computed = 0" */
		p[6] = (unsigned char)((cs >> 8) & 0xFF);
		p[7] = (unsigned char)(cs & 0xFF);
		failures += expect_int("valid checksum: accepted",
		                       udp_checksum_ok(0x0A0B0C0Du, 0x0E0F1011u, p, sizeof(p)), 1);

		/* Flip one body bit -> rejected. */
		p[10] ^= 0x01;
		failures += expect_int("corrupted body: rejected",
		                       udp_checksum_ok(0x0A0B0C0Du, 0x0E0F1011u, p, sizeof(p)), 0);
		p[10] ^= 0x01;                  /* restore */

		/* Wrong src IP -> rejected (pseudo-header mismatch). */
		failures += expect_int("wrong src IP: rejected",
		                       udp_checksum_ok(0x0A0B0C0Eu, 0x0E0F1011u, p, sizeof(p)), 0);
		/* Wrong dst IP -> rejected. */
		failures += expect_int("wrong dst IP: rejected",
		                       udp_checksum_ok(0x0A0B0C0Du, 0x0E0F1012u, p, sizeof(p)), 0);
	}

	/* Odd-length packet: tests the trailing-byte handling. */
	{
		unsigned char p[11] = { 0x00,0x35, 0x00,0x35, 0,11, 0,0, 'h','i','x' };
		unsigned short cs = udp_checksum(0x01010101u, 0x02020202u, p, sizeof(p));
		if (cs == 0) cs = 0xFFFF;
		p[6] = (unsigned char)((cs >> 8) & 0xFF);
		p[7] = (unsigned char)(cs & 0xFF);
		failures += expect_int("odd length: accepted with valid sum",
		                       udp_checksum_ok(0x01010101u, 0x02020202u, p, sizeof(p)), 1);
		p[10] ^= 0xFF;
		failures += expect_int("odd length corrupted: rejected",
		                       udp_checksum_ok(0x01010101u, 0x02020202u, p, sizeof(p)), 0);
	}

	/* Bit-flip in the header itself (length field) -> rejected. */
	{
		unsigned char p[16] = { 0xCC,0xDD, 0xEE,0xFF, 0,16, 0,0, 0,1,2,3,4,5,6,7 };
		unsigned short cs = udp_checksum(0x11111111u, 0x22222222u, p, sizeof(p));
		if (cs == 0) cs = 0xFFFF;
		p[6] = (unsigned char)((cs >> 8) & 0xFF);
		p[7] = (unsigned char)(cs & 0xFF);
		p[5] ^= 0x01;                   /* corrupt header length */
		failures += expect_int("header corruption: rejected",
		                       udp_checksum_ok(0x11111111u, 0x22222222u, p, sizeof(p)), 0);
	}

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
