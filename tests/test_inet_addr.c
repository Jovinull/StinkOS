/* Host-side unit test for inet_addr / htons_b / htonl_b. The BSD wrapper
 * is enough of a pure-userland routine that we can compile it against host
 * gcc, no stubs required. Verifies dotted-quad parsing and byte-swap
 * helpers against hand-computed expected values. */
#include <stdio.h>
#include "../lib/libstink_socket.h"

extern unsigned int   htonl_b(unsigned int v);
extern unsigned short htons_b(unsigned short v);
extern in_addr_t      inet_addr(const char *cp);

static int expect_u32(const char *label, unsigned int got, unsigned int want)
{
	if (got == want) {
		printf("ok   %s = 0x%08x\n", label, got);
		return 0;
	}
	printf("FAIL %s: got 0x%08x, want 0x%08x\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;

	failures += expect_u32("htons_b 0x1234",     htons_b(0x1234),     0x3412);
	failures += expect_u32("htons_b 0x00FF",     htons_b(0x00FF),     0xFF00);
	failures += expect_u32("htonl_b 0x12345678", htonl_b(0x12345678), 0x78563412);
	failures += expect_u32("htonl_b 0xDEADBEEF", htonl_b(0xDEADBEEF), 0xEFBEADDE);

	/* inet_addr returns network byte order: 192.168.1.10 ->
	 * bytes 192, 168, 1, 10 in increasing memory address, which on
	 * little-endian load yields 0x0A01A8C0. */
	failures += expect_u32("inet_addr 192.168.1.10", inet_addr("192.168.1.10"),
	                       0x0A01A8C0);
	failures += expect_u32("inet_addr 0.0.0.0",       inet_addr("0.0.0.0"),
	                       0x00000000);
	failures += expect_u32("inet_addr 255.255.255.255",
	                       inet_addr("255.255.255.255"), 0xFFFFFFFF);
	failures += expect_u32("inet_addr empty",         inet_addr(""),         0);
	failures += expect_u32("inet_addr garbage",       inet_addr("not.an.ip"), 0);
	failures += expect_u32("inet_addr overflow",      inet_addr("999.0.0.1"), 0);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
