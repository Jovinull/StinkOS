/* Host-side fuzz / negative test for the IPv4 header validator in
 * kernel/drivers/net/ipv4.c (`ip_handle`'s defensive checks). We
 * replicate the early-reject logic here and prove every malformed shape
 * exits without dereferencing past the buffer.
 */
#include <stdio.h>
#include <string.h>

/* Mirror of the prologue in ip_handle(): returns 1 if the IPv4 header is
 * acceptable to dispatch, 0 if it must be silently dropped. No payload
 * dispatch -- that lives downstream and gets its own tests. Bytes are
 * already in network order on the wire; rebuild u16 fields directly. */
static int ipv4_header_ok(const unsigned char *p, unsigned int len)
{
	if (len < 20) return 0;
	if ((p[0] >> 4) != 4) return 0;
	unsigned int ihl = (p[0] & 0x0F) * 4u;
	if (ihl < 20 || ihl > len) return 0;
	unsigned int total = ((unsigned int)p[2] << 8) | p[3];
	if (total > len || total < ihl) return 0;
	return 1;
}

static int expect(const char *label, int got, int want)
{
	if (got == want) {
		printf("ok   %-30s = %d\n", label, got);
		return 0;
	}
	printf("FAIL %s: got %d, want %d\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;

	unsigned char good[20] = {
		0x45, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00,
		0x40, 0x06, 0x00, 0x00, 0xAC, 0x10, 0x0A, 0x63,
		0xAC, 0x10, 0x0A, 0x0C,
	};
	failures += expect("good 20-byte header", ipv4_header_ok(good, 20), 1);

	/* Truncated: only first byte. */
	failures += expect("len = 1 byte",        ipv4_header_ok(good, 1),  0);
	failures += expect("len = 19 (one short)", ipv4_header_ok(good, 19), 0);

	/* Wrong version. */
	{
		unsigned char b[20]; memcpy(b, good, 20);
		b[0] = 0x65;          /* version 6 in nibble 1 */
		failures += expect("version=6",     ipv4_header_ok(b, 20), 0);
		b[0] = 0x00;          /* version 0 */
		failures += expect("version=0",     ipv4_header_ok(b, 20), 0);
	}

	/* IHL < 5 (would yield ihl < 20). */
	{
		unsigned char b[20]; memcpy(b, good, 20);
		b[0] = 0x44;          /* version 4, IHL 4 -> 16 bytes */
		failures += expect("ihl=4 (too small)", ipv4_header_ok(b, 20), 0);
	}

	/* IHL larger than buffer. */
	{
		unsigned char b[20]; memcpy(b, good, 20);
		b[0] = 0x4F;          /* version 4, IHL 15 -> 60 bytes */
		failures += expect("ihl=15 vs buf 20", ipv4_header_ok(b, 20), 0);
	}

	/* total_length larger than buffer. */
	{
		unsigned char b[20]; memcpy(b, good, 20);
		b[2] = 0xFF; b[3] = 0xFF;
		failures += expect("total > len",      ipv4_header_ok(b, 20), 0);
	}

	/* total_length smaller than ihl. */
	{
		unsigned char b[20]; memcpy(b, good, 20);
		b[2] = 0x00; b[3] = 0x0A;       /* total = 10 < ihl 20 */
		failures += expect("total < ihl",      ipv4_header_ok(b, 20), 0);
	}

	/* Zero-length buffer. */
	failures += expect("len = 0", ipv4_header_ok((unsigned char *)"", 0), 0);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
