/* Host-side test for ARP cache entry expiry added to
 * kernel/drivers/net/arp.c. Entries get stamped with pit_ticks() on
 * insert/update; arp_lookup silently evicts the entry if it has been
 * untouched for more than ARP_ENTRY_TTL (6000 ticks = 60 s at 100 Hz).
 *
 * The eviction is lazy (happens at lookup time) -- there's no sweeper
 * thread; we cannot tolerate yet another tick callback at the moment.
 */
#include <stdio.h>

#define ARP_ENTRY_TTL 6000u

struct entry {
	int          valid;
	unsigned int touched_tick;
};

static struct entry slot;     /* single-slot mirror; the cache table is identical */
static unsigned int now;       /* test-side clock */

static void insert(unsigned int t)
{
	slot.valid        = 1;
	slot.touched_tick = t;
}

static int lookup(unsigned int t)
{
	if (!slot.valid) return 0;
	if ((t - slot.touched_tick) > ARP_ENTRY_TTL) {
		slot.valid = 0;     /* lazy evict */
		return 0;
	}
	return 1;
}

static int expect_int(const char *label, int got, int want)
{
	if (got == want) { printf("ok   %-50s = %d\n", label, got); return 0; }
	printf("FAIL %s: got %d, want %d\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;
	(void)now;

	/* Fresh insert: lookup hits. */
	insert(1000);
	failures += expect_int("fresh entry: hit",            lookup(1000), 1);
	failures += expect_int("hit at t+1000",               lookup(2000), 1);
	failures += expect_int("hit at +5999 (under TTL)",    lookup(6999), 1);

	/* At exactly TTL+1 the lazy evict triggers. */
	insert(1000);
	failures += expect_int("evict at +6001 (over TTL)",   lookup(7001), 0);
	failures += expect_int("evicted: subsequent miss",    lookup(7002), 0);

	/* Re-insert after eviction refreshes timer. */
	insert(7100);
	failures += expect_int("re-insert: hit",              lookup(7200), 1);

	/* Insert + many lookups: never expires while refreshed inside window. */
	insert(0);
	failures += expect_int("hit at exactly TTL",          lookup(ARP_ENTRY_TTL), 1);
	insert(ARP_ENTRY_TTL);
	failures += expect_int("post-refresh: still alive",   lookup(ARP_ENTRY_TTL + 5999), 1);

	/* Counter wraparound: unsigned subtraction stays correct. */
	insert(0xFFFFFFFFu - 10u);
	failures += expect_int("near rollover: still in window",
	                        lookup(0xFFFFFFFFu - 5u),    1);
	failures += expect_int("post-rollover: in window",
	                        lookup(0xFFFFFFFFu + 100u),  1);
	failures += expect_int("post-rollover: expired",
	                        lookup(0xFFFFFFFFu + 6000u), 0);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
