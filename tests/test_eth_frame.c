/* Host-side test for the Ethernet II frame-length guard in
 * kernel/drivers/net/ethernet.c (`eth_handle_frame`). The dispatch path
 * MUST drop frames shorter than the 14-byte header AND frames larger
 * than 14 + 1500 = 1514 bytes (the NIC has already stripped the 4-byte
 * FCS by the time we see the frame; this is NOT 1518). Letting either
 * extreme through would feed corrupt lengths to ARP / IPv4 parsing and
 * either read past the buffer or interpret garbage as a valid packet.
 *
 * The replica below tracks the kernel logic exactly: a frame is
 * dispatchable iff ETH_HDR_LEN <= len <= ETH_MAX_FRAME, regardless of
 * ethertype. Anything else returns 0 (= drop).
 */
#include <stdio.h>

#define ETH_HDR_LEN     14
#define ETH_MAX_PAYLOAD 1500
#define ETH_MAX_FRAME   (ETH_HDR_LEN + ETH_MAX_PAYLOAD)

static int eth_dispatchable(unsigned int len)
{
	if (len < ETH_HDR_LEN || len > ETH_MAX_FRAME)
		return 0;
	return 1;
}

static int expect(const char *label, int got, int want)
{
	if (got == want) { printf("ok   %-40s = %d\n", label, got); return 0; }
	printf("FAIL %s: got %d, want %d\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;

	/* Boundary: empty buffer is dropped. */
	failures += expect("len 0 dropped",                   eth_dispatchable(0),     0);

	/* Boundary: 13 bytes is one short of a complete header. */
	failures += expect("len 13 dropped",                  eth_dispatchable(13),    0);

	/* Boundary: 14 bytes is the smallest dispatchable frame -- header
	 * only, zero-byte payload (rare but well-formed). */
	failures += expect("len 14 (header only) accepted",   eth_dispatchable(14),    1);

	/* Typical small frames. */
	failures += expect("len 42 (ARP request) accepted",   eth_dispatchable(42),    1);
	failures += expect("len 64 (min Ethernet) accepted",  eth_dispatchable(64),    1);
	failures += expect("len 60 accepted",                 eth_dispatchable(60),    1);

	/* Typical large frames just under MTU. */
	failures += expect("len 1500 accepted",               eth_dispatchable(1500),  1);
	failures += expect("len 1513 accepted",               eth_dispatchable(1513),  1);

	/* Boundary: exactly 1514 (header + 1500 payload, FCS stripped) is
	 * the largest legal standard frame the dispatch layer should see. */
	failures += expect("len 1514 (max MTU) accepted",     eth_dispatchable(1514),  1);

	/* Boundary: 1515 is the first byte past MTU and must be dropped --
	 * a jumbo frame leaking through here would let upper layers parse
	 * fields past their declared lengths. */
	failures += expect("len 1515 dropped",                eth_dispatchable(1515),  0);
	failures += expect("len 1518 (incl FCS) dropped",     eth_dispatchable(1518),  0);

	/* Pathological oversized frames. */
	failures += expect("len 2000 dropped",                eth_dispatchable(2000),  0);
	failures += expect("len 9000 (jumbo) dropped",        eth_dispatchable(9000),  0);
	failures += expect("len 65535 dropped",               eth_dispatchable(65535), 0);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
