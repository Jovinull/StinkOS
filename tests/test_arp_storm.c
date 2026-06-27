/* Host-side test for the ARP receive path in kernel/drivers/net/arp.c
 * (`arp_handle` + `cache_insert`) under storm conditions. Replicates the
 * validation guards and the cache-insert side-effect, then drives a flood
 * of well-formed and malformed ARP packets to make sure:
 *
 *   - malformed packets (short, wrong hw_type, wrong proto_type, wrong
 *     hw_size / proto_size) never reach the cache;
 *   - a burst of N > cache_size distinct senders fills + wraps the cache
 *     in insertion order (round-robin), keeping the freshest N entries
 *     intact and never duplicating an IP slot;
 *   - a "poisoning" burst that re-claims an existing IP with a new MAC
 *     updates in place rather than evicting an unrelated entry.
 *
 * The kernel logic is replicated here so changes to either side surface
 * as a test diff -- the algorithm rules ARE the test.
 */
#include <stdio.h>
#include <string.h>

#define ARP_CACHE_SIZE 16

struct entry {
	unsigned int  ip;
	unsigned char mac[6];
	int           valid;
};

static struct entry cache[ARP_CACHE_SIZE];
static unsigned int cache_next;

static void cache_reset(void)
{
	for (int i = 0; i < ARP_CACHE_SIZE; i++) cache[i].valid = 0;
	cache_next = 0;
}

static void cache_insert(unsigned int ip, const unsigned char mac[6])
{
	for (int i = 0; i < ARP_CACHE_SIZE; i++) {
		if (cache[i].valid && cache[i].ip == ip) {
			for (int k = 0; k < 6; k++) cache[i].mac[k] = mac[k];
			return;
		}
	}
	int slot = -1;
	for (int i = 0; i < ARP_CACHE_SIZE; i++) {
		if (!cache[i].valid) { slot = i; break; }
	}
	if (slot < 0) {
		slot = (int)cache_next;
		cache_next = (cache_next + 1) % ARP_CACHE_SIZE;
	}
	cache[slot].ip = ip;
	for (int k = 0; k < 6; k++) cache[slot].mac[k] = mac[k];
	cache[slot].valid = 1;
}

static int cache_lookup(unsigned int ip, unsigned char out[6])
{
	for (int i = 0; i < ARP_CACHE_SIZE; i++) {
		if (cache[i].valid && cache[i].ip == ip) {
			for (int k = 0; k < 6; k++) out[k] = cache[i].mac[k];
			return 1;
		}
	}
	return 0;
}

static unsigned int cache_valid_count(void)
{
	unsigned int n = 0;
	for (int i = 0; i < ARP_CACHE_SIZE; i++) if (cache[i].valid) n++;
	return n;
}

/* Returns 1 if the packet would teach the cache; 0 if dropped at validation.
 * Mirrors arp_handle (length, hw_type, proto_type, hw_size, proto_size). */
static int arp_validate(unsigned int len,
                        unsigned short hw_type, unsigned short proto_type,
                        unsigned char  hw_size, unsigned char  proto_size)
{
	if (len < 28)                         return 0;   /* sizeof(arp_packet) */
	if (hw_type    != 1)                  return 0;   /* Ethernet */
	if (proto_type != 0x0800)             return 0;   /* IPv4 */
	if (hw_size    != 6)                  return 0;
	if (proto_size != 4)                  return 0;
	return 1;
}

static int expect_int(const char *label, int got, int want)
{
	if (got == want) { printf("ok   %-50s = %d\n", label, got); return 0; }
	printf("FAIL %s: got %d, want %d\n", label, got, want);
	return 1;
}

static int expect_mac(const char *label, const unsigned char got[6],
                      const unsigned char want[6])
{
	if (memcmp(got, want, 6) == 0) { printf("ok   %-50s\n", label); return 0; }
	printf("FAIL %s: MAC mismatch\n", label);
	return 1;
}

int main(void)
{
	int failures = 0;
	unsigned char dummy[6] = {1, 2, 3, 4, 5, 6};

	/* --- validation guards -------------------------------------------- */
	failures += expect_int("len < 28 rejected",       arp_validate(27, 1, 0x0800, 6, 4), 0);
	failures += expect_int("len = 28 accepted",       arp_validate(28, 1, 0x0800, 6, 4), 1);
	failures += expect_int("wrong hw_type rejected",  arp_validate(28, 2, 0x0800, 6, 4), 0);
	failures += expect_int("wrong proto rejected",    arp_validate(28, 1, 0x86DD, 6, 4), 0);
	failures += expect_int("wrong hw_size rejected",  arp_validate(28, 1, 0x0800, 8, 4), 0);
	failures += expect_int("wrong proto_size rejected",arp_validate(28, 1, 0x0800, 6, 16),0);
	failures += expect_int("oversized still accepted",arp_validate(60, 1, 0x0800, 6, 4), 1);

	/* --- malformed storm: 1000 invalid frames must NOT touch cache --- */
	cache_reset();
	for (int i = 0; i < 1000; i++) {
		if (arp_validate(20, 1, 0x0800, 6, 4))           cache_insert(i, dummy);
		if (arp_validate(28, 99, 0x0800, 6, 4))          cache_insert(i, dummy);
		if (arp_validate(28, 1, 0xBEEF, 6, 4))           cache_insert(i, dummy);
	}
	failures += expect_int("malformed storm leaves cache empty", (int)cache_valid_count(), 0);

	/* --- well-formed storm: 1000 distinct senders ----------------------
	 * Cache holds 16; after the storm exactly 16 entries are valid and
	 * the IPs present are the LAST 16 senders inserted, since round-robin
	 * eviction starts at slot 0 once the table is full. */
	cache_reset();
	for (int i = 0; i < 1000; i++) {
		unsigned char mac[6] = { 0xAA, 0xBB, 0xCC,
		                         (unsigned char)(i >> 16),
		                         (unsigned char)(i >> 8),
		                         (unsigned char)i };
		cache_insert((unsigned int)(0x0A000000u + i), mac);
	}
	failures += expect_int("storm leaves cache full",
	                       (int)cache_valid_count(), ARP_CACHE_SIZE);

	/* IPs 0..15 land in slots 0..15. After IP 16 the cursor evicts slot 0
	 * (IP 0), then slot 1 (IP 1), and so on. After 1000 inserts, the
	 * cursor has wrapped many times; the surviving IPs are the most
	 * recent 16, BUT the layout is:
	 *   slot j holds IP (1000 - 16 + ((cursor_after + j) mod 16))
	 * which simplifies to: the 16 most-recent IPs are present.  Easier
	 * to verify by checking presence/absence, not slot indices. */
	unsigned char tmp[6];
	for (int recent = 1000 - ARP_CACHE_SIZE; recent < 1000; recent++) {
		if (!cache_lookup((unsigned int)(0x0A000000u + recent), tmp)) {
			printf("FAIL recent IP %d evicted\n", recent);
			failures++;
		}
	}
	failures += expect_int("oldest IP (0) evicted",
	                       cache_lookup(0x0A000000u, tmp), 0);
	failures += expect_int("mid-range IP (500) evicted",
	                       cache_lookup(0x0A000000u + 500, tmp), 0);

	/* --- poisoning storm: re-claim an existing IP with a new MAC ----
	 * MUST update in place. If it evicted instead, cache size would still
	 * be 16 but one of the other 15 IPs would be gone. */
	cache_reset();
	for (int i = 0; i < ARP_CACHE_SIZE; i++) {
		unsigned char m[6] = {0, 0, 0, 0, 0, (unsigned char)i};
		cache_insert(0x10000000u + i, m);
	}
	unsigned char poison[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};
	for (int rep = 0; rep < 500; rep++)
		cache_insert(0x10000005u, poison);
	failures += expect_int("poison: cache still full",
	                       (int)cache_valid_count(), ARP_CACHE_SIZE);
	failures += expect_int("poison: target IP still present",
	                       cache_lookup(0x10000005u, tmp), 1);
	failures += expect_mac("poison: MAC updated in place", tmp, poison);
	/* And no innocent neighbour was evicted. */
	for (int i = 0; i < ARP_CACHE_SIZE; i++) {
		if (!cache_lookup(0x10000000u + i, tmp)) {
			printf("FAIL poison evicted IP slot %d\n", i);
			failures++;
		}
	}

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
