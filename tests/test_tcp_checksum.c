/* Host-side test for the TCP segment checksum verifier added at the top
 * of tcp_handle in kernel/drivers/net/tcp.c. Without it, a flipped bit
 * on the wire could shift seq/ack values or inject bytes into an open
 * connection -- TCP has no other integrity check below TLS.
 *
 * Algorithm (RFC 793 §3.1 / RFC 1071):
 *   sum = one's-complement add over pseudo-header + segment-as-is
 *         (with the existing checksum field included)
 *   accept iff sum folds to 0xFFFF
 */
#include <stdio.h>

#define IP_PROTO_TCP 6

static int tcp_checksum_ok(unsigned int src, unsigned int dst,
                           const unsigned char *seg, unsigned int seg_len)
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
	ph[9] = IP_PROTO_TCP;
	ph[10] = (unsigned char)((seg_len >> 8) & 0xFF);
	ph[11] = (unsigned char)( seg_len       & 0xFF);

	for (unsigned int i = 0; i + 1 < sizeof(ph); i += 2)
		sum += ((unsigned int)ph[i] << 8) | ph[i + 1];

	for (unsigned int i = 0; i + 1 < seg_len; i += 2)
		sum += ((unsigned int)seg[i] << 8) | seg[i + 1];
	if (seg_len & 1)
		sum += (unsigned int)seg[seg_len - 1] << 8;

	while (sum >> 16)
		sum = (sum & 0xFFFFu) + (sum >> 16);
	return (sum & 0xFFFFu) == 0xFFFFu;
}

/* Sender-side checksum (writes a placeholder into seg[16..17]). The
 * checksum field for TCP lives at offset 16 in the header. */
static unsigned short tcp_checksum_compute(unsigned int src, unsigned int dst,
                                           const unsigned char *seg, unsigned int seg_len)
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
	ph[9] = IP_PROTO_TCP;
	ph[10] = (unsigned char)((seg_len >> 8) & 0xFF);
	ph[11] = (unsigned char)( seg_len       & 0xFF);

	for (unsigned int i = 0; i + 1 < sizeof(ph); i += 2)
		sum += ((unsigned int)ph[i] << 8) | ph[i + 1];

	for (unsigned int i = 0; i + 1 < seg_len; i += 2) {
		if (i == 16) continue;     /* skip checksum field at offset 16 */
		sum += ((unsigned int)seg[i] << 8) | seg[i + 1];
	}
	if (seg_len & 1)
		sum += (unsigned int)seg[seg_len - 1] << 8;

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

static void build_seg(unsigned char *seg, unsigned int *seq, unsigned int *ack)
{
	(void)ack;
	/* 20-byte TCP header + 8 bytes payload */
	seg[0] = 0x12; seg[1] = 0x34;                /* src_port */
	seg[2] = 0x00; seg[3] = 0x50;                /* dst_port = 80 */
	seg[4] = (unsigned char)((*seq >> 24) & 0xFF);
	seg[5] = (unsigned char)((*seq >> 16) & 0xFF);
	seg[6] = (unsigned char)((*seq >>  8) & 0xFF);
	seg[7] = (unsigned char)( *seq        & 0xFF);
	seg[8] = 0; seg[9] = 0; seg[10] = 0; seg[11] = 0;   /* ack=0 */
	seg[12] = 0x50;                              /* data_off = 5 dwords */
	seg[13] = 0x10;                              /* flags = ACK */
	seg[14] = 0x10; seg[15] = 0x00;              /* window */
	seg[16] = 0; seg[17] = 0;                    /* checksum (placeholder) */
	seg[18] = 0; seg[19] = 0;                    /* urgent */
	seg[20] = 'h'; seg[21] = 'e'; seg[22] = 'l'; seg[23] = 'l';
	seg[24] = 'o'; seg[25] = '!'; seg[26] = '!'; seg[27] = '!';
}

int main(void)
{
	int failures = 0;
	unsigned char seg[32];
	unsigned int seq = 0xDEADBEEFu, ack = 0;
	unsigned int src = 0x0A0B0C0Du, dst = 0x05060708u;

	/* Valid segment. */
	build_seg(seg, &seq, &ack);
	unsigned short cs = tcp_checksum_compute(src, dst, seg, 28);
	seg[16] = (unsigned char)((cs >> 8) & 0xFF);
	seg[17] = (unsigned char)(cs & 0xFF);
	failures += expect_int("valid: accepted",            tcp_checksum_ok(src, dst, seg, 28), 1);

	/* Flip body bit -> rejected. */
	seg[20] ^= 0x01;
	failures += expect_int("body bit flipped: rejected", tcp_checksum_ok(src, dst, seg, 28), 0);
	seg[20] ^= 0x01;

	/* Flip seq bit -> rejected (this is the attack scenario). */
	seg[5] ^= 0x80;
	failures += expect_int("seq bit flipped: rejected",  tcp_checksum_ok(src, dst, seg, 28), 0);
	seg[5] ^= 0x80;

	/* Wrong src IP -> rejected (pseudo-header mismatch). */
	failures += expect_int("wrong src IP: rejected",     tcp_checksum_ok(0x0A0B0C0Eu, dst, seg, 28), 0);

	/* Wrong dst IP -> rejected. */
	failures += expect_int("wrong dst IP: rejected",     tcp_checksum_ok(src, 0x05060709u, seg, 28), 0);

	/* All-zero checksum field (unconfigured sender): would only validate
	 * if the rest happens to sum to 0xFFFF, which it does not for this
	 * segment. The kernel does NOT exempt zero TCP checksums (unlike UDP),
	 * so a stack that forgets to compute is dropped. */
	seg[16] = 0; seg[17] = 0;
	failures += expect_int("zero checksum: rejected",    tcp_checksum_ok(src, dst, seg, 28), 0);

	/* Odd-length segment trailing-byte handling. */
	{
		unsigned char s[21];
		for (int i = 0; i < 21; i++) s[i] = (unsigned char)i;
		s[12] = 0x50; s[13] = 0x10;
		s[16] = 0; s[17] = 0;
		unsigned short c = tcp_checksum_compute(src, dst, s, 21);
		s[16] = (unsigned char)((c >> 8) & 0xFF);
		s[17] = (unsigned char)(c & 0xFF);
		failures += expect_int("odd length: accepted",   tcp_checksum_ok(src, dst, s, 21), 1);
		s[20] ^= 0xFF;
		failures += expect_int("odd length corrupted: rejected",
		                       tcp_checksum_ok(src, dst, s, 21), 0);
	}

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
