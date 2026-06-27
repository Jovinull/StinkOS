/* Host-side test for the ARP cache logic in kernel/drivers/net/arp.c
 * (`cache_insert` + `arp_lookup`). The cache is a 16-entry round-robin
 * with update-in-place on existing IPs and free-slot-first on inserts.
 * Algorithm replica kept in sync by hand; any change to cache_insert
 * should land here too.
 */
#include <stdio.h>

#define ARP_CACHE_SIZE 16

struct entry {
	unsigned int ip;
	unsigned char mac[6];
	int valid;
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

static int expect_int(const char *label, int got, int want)
{
	if (got == want) { printf("ok   %-40s = %d\n", label, got); return 0; }
	printf("FAIL %s: got %d, want %d\n", label, got, want);
	return 1;
}

static int expect_mac(const char *label, const unsigned char got[6],
                      const unsigned char want[6])
{
	for (int k = 0; k < 6; k++) if (got[k] != want[k]) {
		printf("FAIL %s: byte %d differs\n", label, k);
		return 1;
	}
	printf("ok   %-40s\n", label);
	return 0;
}

int main(void)
{
	int failures = 0;

	cache_reset();

	/* Empty cache: lookup misses. */
	unsigned char m[6];
	failures += expect_int("empty miss", cache_lookup(0x01020304, m), 0);

	/* Insert + lookup hit. */
	unsigned char macA[6] = {1,2,3,4,5,6};
	cache_insert(0x01020304, macA);
	failures += expect_int("hit after insert", cache_lookup(0x01020304, m), 1);
	failures += expect_mac("hit returns MAC", m, macA);

	/* Update-in-place: same IP, new MAC. */
	unsigned char macB[6] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};
	cache_insert(0x01020304, macB);
	cache_lookup(0x01020304, m);
	failures += expect_mac("update-in-place", m, macB);

	/* Fill cache to capacity (15 more entries, slot 0 already used). */
	cache_reset();
	for (int i = 0; i < ARP_CACHE_SIZE; i++) {
		unsigned char fake[6] = {0,0,0,0,0,(unsigned char)i};
		cache_insert((unsigned int)(0x10000000u + i), fake);
	}
	for (int i = 0; i < ARP_CACHE_SIZE; i++) {
		failures += expect_int("filled cache hit",
		                       cache_lookup((unsigned int)(0x10000000u + i), m), 1);
	}

	/* Overflow: round-robin evicts slot 0 first. */
	unsigned char overflow_mac[6] = {0,0,0,0,0,42};
	cache_insert(0x20000000u, overflow_mac);
	/* The original first IP (0x10000000) should be evicted; everything
	 * else still there. */
	failures += expect_int("overflow evicts slot 0",
	                       cache_lookup(0x10000000u, m), 0);
	failures += expect_int("overflow keeps slot 1",
	                       cache_lookup(0x10000001u, m), 1);
	failures += expect_int("overflow new IP present",
	                       cache_lookup(0x20000000u, m), 1);

	/* Continued overflow: next eviction is slot 1. */
	cache_insert(0x20000001u, overflow_mac);
	failures += expect_int("second overflow evicts slot 1",
	                       cache_lookup(0x10000001u, m), 0);
	failures += expect_int("second overflow keeps slot 2",
	                       cache_lookup(0x10000002u, m), 1);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
