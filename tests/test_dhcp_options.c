/* Host-side test for the DHCP options parser in kernel/drivers/net/dhcp.c
 * (`parse_options`). TLV stream: code byte, length byte, payload bytes.
 * Codes 0 (PAD) and 255 (END) are bare (no length). We replicate the
 * loop here and check the specific options the StinkOS DHCP client
 * actually consumes: subnet mask, router, DNS, server-id, msg-type.
 */
#include <stdio.h>
#include <string.h>

#define OPT_PAD          0
#define OPT_MSG_TYPE     53
#define OPT_SUBNET_MASK  1
#define OPT_ROUTER       3
#define OPT_DNS          6
#define OPT_SERVER_ID    54
#define OPT_END          255

struct dhcp_out {
	unsigned char  msg_type;
	unsigned int   subnet;
	unsigned int   router;
	unsigned int   dns;
	unsigned int   server;
};

static unsigned int read_ipv4_le(const unsigned char *p)
{
	/* DHCP wire layout: dotted-quad bytes in transmission order =>
	 * stored as a u32 in network byte order, which on little-endian
	 * x86 becomes bytes[0] in lowest position. */
	return  (unsigned int)p[0]        |
	       ((unsigned int)p[1] <<  8) |
	       ((unsigned int)p[2] << 16) |
	       ((unsigned int)p[3] << 24);
}

static void parse(const unsigned char *opts, unsigned int len,
                  struct dhcp_out *o)
{
	memset(o, 0, sizeof(*o));
	unsigned int i = 0;
	while (i < len) {
		unsigned char code = opts[i++];
		if (code == OPT_END) break;
		if (code == OPT_PAD) continue;
		if (i >= len) break;
		unsigned char olen = opts[i++];
		if (i + olen > len) break;
		switch (code) {
		case OPT_MSG_TYPE:    if (olen >= 1) o->msg_type = opts[i]; break;
		case OPT_SUBNET_MASK: if (olen == 4) o->subnet   = read_ipv4_le(&opts[i]); break;
		case OPT_ROUTER:      if (olen >= 4) o->router   = read_ipv4_le(&opts[i]); break;
		case OPT_DNS:         if (olen >= 4) o->dns      = read_ipv4_le(&opts[i]); break;
		case OPT_SERVER_ID:   if (olen == 4) o->server   = read_ipv4_le(&opts[i]); break;
		default: break;
		}
		i += olen;
	}
}

static int expect_u32(const char *l, unsigned int got, unsigned int want)
{
	if (got == want) { printf("ok   %-40s = 0x%08x\n", l, got); return 0; }
	printf("FAIL %s: got 0x%08x, want 0x%08x\n", l, got, want);
	return 1;
}

static int expect_int(const char *l, int got, int want)
{
	if (got == want) { printf("ok   %-40s = %d\n", l, got); return 0; }
	printf("FAIL %s: got %d, want %d\n", l, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;
	struct dhcp_out o;

	/* Empty option block: nothing set. */
	parse((unsigned char *)"", 0, &o);
	failures += expect_int("empty msg_type", (int)o.msg_type, 0);
	failures += expect_u32("empty subnet",   o.subnet, 0);

	/* PADs only -> still nothing. */
	{
		unsigned char p[3] = {0, 0, 0};
		parse(p, 3, &o);
		failures += expect_int("only PADs msg_type", (int)o.msg_type, 0);
	}

	/* Full options block ending in OPT_END. */
	{
		unsigned char p[] = {
			53, 1, 5,                           /* msg_type = DHCPACK */
			 1, 4, 255, 255, 255, 0,             /* subnet 255.255.255.0 */
			 3, 4, 192, 168,   1,   1,           /* router 192.168.1.1 */
			 6, 4,   8,   8,   8,   8,           /* DNS 8.8.8.8 */
			54, 4, 192, 168,   1,   1,           /* server-id 192.168.1.1 */
			255                                  /* OPT_END */
		};
		parse(p, sizeof(p), &o);
		failures += expect_int("DHCPACK msg_type",  (int)o.msg_type, 5);
		failures += expect_u32("subnet 255.255.255.0", o.subnet, 0x00FFFFFFu);
		failures += expect_u32("router 192.168.1.1",   o.router, 0x0101A8C0u);
		failures += expect_u32("DNS 8.8.8.8",          o.dns,    0x08080808u);
		failures += expect_u32("server-id 192.168.1.1",o.server, 0x0101A8C0u);
	}

	/* Truncated option -- length runs past buffer. Parser must abort
	 * cleanly without dereferencing past the end. */
	{
		unsigned char p[] = { 1, 8, 1, 2, 3 };   /* claims 8 bytes, has 3 */
		parse(p, sizeof(p), &o);
		failures += expect_u32("truncated option ignored", o.subnet, 0);
	}

	/* Unknown option (code 99, len 2) should be skipped without affecting
	 * known options after it. */
	{
		unsigned char p[] = { 99, 2, 0xAA, 0xBB, 53, 1, 2, 255 };
		parse(p, sizeof(p), &o);
		failures += expect_int("known after unknown",
		                       (int)o.msg_type, 2);
	}

	/* Wrong length on subnet (3 bytes) ignored; should stay 0. */
	{
		unsigned char p[] = { 1, 3, 0xFF, 0xFF, 0xFF, 255 };
		parse(p, sizeof(p), &o);
		failures += expect_u32("bad subnet length ignored", o.subnet, 0);
	}

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
