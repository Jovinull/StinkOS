/* Host-side test for the ICMP echo reply path in
 * kernel/drivers/net/icmp.c. The kernel mirrors an incoming echo
 * request back to its sender with:
 *   - type swapped from ECHO_REQUEST (8) to ECHO_REPLY (0)
 *   - identifier + sequence preserved verbatim
 *   - payload preserved verbatim
 *   - checksum recomputed (sum folds to 0xFFFF on the way out)
 *
 * The mirror logic is trivial but the checksum recompute is where
 * silent regressions show up -- if we accidentally include the old
 * type byte in the new sum, the peer sees a "bad checksum" reply.
 */
#include <stdio.h>

/* ICMP RFC 792 one's-complement sum (same as ipv4_checksum in the
 * kernel -- not pseudo-header'd; just folds the message bytes). */
static unsigned short icmp_checksum(const unsigned char *p, unsigned int len)
{
	unsigned int sum = 0;
	for (unsigned int i = 0; i + 1 < len; i += 2)
		sum += ((unsigned int)p[i] << 8) | p[i + 1];
	if (len & 1)
		sum += (unsigned int)p[len - 1] << 8;
	while (sum >> 16)
		sum = (sum & 0xFFFFu) + (sum >> 16);
	return (unsigned short)(~sum & 0xFFFFu);
}

/* Mirror of the kernel's reply builder: copy verbatim, swap type byte,
 * zero the checksum field, recompute. */
static void build_reply(unsigned char *dst, const unsigned char *req, unsigned int len)
{
	for (unsigned int i = 0; i < len; i++) dst[i] = req[i];
	dst[0] = 0;          /* ECHO_REPLY */
	dst[2] = 0;          /* checksum hi cleared */
	dst[3] = 0;          /* checksum lo cleared */
	unsigned short cs = icmp_checksum(dst, len);
	dst[2] = (unsigned char)((cs >> 8) & 0xFF);
	dst[3] = (unsigned char)(cs & 0xFF);
}

static int expect_uint(const char *label, unsigned int got, unsigned int want)
{
	if (got == want) { printf("ok   %-50s = 0x%04X\n", label, got); return 0; }
	printf("FAIL %s: got 0x%X, want 0x%X\n", label, got, want);
	return 1;
}

static int expect_buf_eq(const char *label,
                         const unsigned char *got, const unsigned char *want,
                         unsigned int len, unsigned int skip_start, unsigned int skip_end)
{
	for (unsigned int i = 0; i < len; i++) {
		if (i >= skip_start && i < skip_end) continue;
		if (got[i] != want[i]) {
			printf("FAIL %s: byte %u got 0x%02X want 0x%02X\n",
			       label, i, got[i], want[i]);
			return 1;
		}
	}
	printf("ok   %-50s\n", label);
	return 0;
}

int main(void)
{
	int failures = 0;

	/* Build a 24-byte echo request: 8B ICMP header + 16B payload. */
	unsigned char req[24];
	req[0] = 8;                          /* type = ECHO_REQUEST */
	req[1] = 0;                          /* code */
	req[2] = 0; req[3] = 0;              /* checksum placeholder */
	req[4] = 0x12; req[5] = 0x34;        /* identifier */
	req[6] = 0xAB; req[7] = 0xCD;        /* sequence */
	for (int i = 0; i < 16; i++) req[8 + i] = (unsigned char)('a' + i);
	unsigned short req_cs = icmp_checksum(req, sizeof(req));
	req[2] = (unsigned char)((req_cs >> 8) & 0xFF);
	req[3] = (unsigned char)(req_cs & 0xFF);

	/* Verify the request checksum closes. */
	failures += expect_uint("request: checksum closes (~sum = 0)",
	                        (unsigned int)icmp_checksum(req, sizeof(req)), 0);

	unsigned char rep[24];
	build_reply(rep, req, sizeof(req));

	/* Type swapped. */
	failures += expect_uint("reply: type = 0 (ECHO_REPLY)", rep[0], 0);
	failures += expect_uint("reply: code unchanged",        rep[1], req[1]);

	/* Identifier + sequence preserved. */
	failures += expect_uint("reply: id hi preserved", rep[4], req[4]);
	failures += expect_uint("reply: id lo preserved", rep[5], req[5]);
	failures += expect_uint("reply: seq hi preserved", rep[6], req[6]);
	failures += expect_uint("reply: seq lo preserved", rep[7], req[7]);

	/* Payload preserved verbatim. */
	failures += expect_buf_eq("reply: payload bytes preserved",
	                          rep, req, sizeof(req), 0, 4);  /* skip type+csum */

	/* Reply checksum closes (peer's verifier returns 0). */
	failures += expect_uint("reply: checksum closes",
	                        (unsigned int)icmp_checksum(rep, sizeof(rep)), 0);

	/* Reply checksum differs from request checksum (type byte changed). */
	unsigned short rep_cs = (unsigned short)((rep[2] << 8) | rep[3]);
	if (rep_cs == req_cs) {
		printf("FAIL reply checksum should differ from request "
		       "(type byte changed)\n");
		failures++;
	} else {
		printf("ok   reply checksum differs from request (type 8->0)\n");
	}

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
