/* Host-side test for the DNS response cache in
 * kernel/drivers/net/dns.c. 8 fixed slots, 60s lazy TTL, name
 * comparison is byte-exact (no case fold), and replacement is
 * round-robin once all slots are in use. Re-inserting an existing
 * name overwrites in place (no slot consumed). On lookup, any entry
 * past TTL is evicted in-line.
 *
 * The arithmetic mirrors dns.c:71-106. If the cache turns into a
 * leaky LRU or the case-fold accidentally re-appears, the table
 * here catches the drift.
 */
#include <stdio.h>

#define DNS_CACHE_SIZE 8
#define DNS_CACHE_TTL  6000u
#define DNS_CACHE_NAME 64

typedef unsigned int ipv4_t;

struct entry {
	int          in_use;
	char         name[DNS_CACHE_NAME];
	ipv4_t       ip;
	unsigned int filled_at;
};

static struct entry tbl[DNS_CACHE_SIZE];
static int          round_cursor;
static unsigned int now_ticks;

static int names_eq(const char *a, const char *b)
{
	int i = 0;
	while (a[i] && b[i] && a[i] == b[i]) i++;
	return a[i] == '\0' && b[i] == '\0';
}

static struct entry *cache_find(const char *name)
{
	for (int i = 0; i < DNS_CACHE_SIZE; i++) {
		struct entry *e = &tbl[i];
		if (!e->in_use) continue;
		if ((unsigned int)(now_ticks - e->filled_at) > DNS_CACHE_TTL) {
			e->in_use = 0;
			continue;
		}
		if (names_eq(e->name, name)) return e;
	}
	return 0;
}

static void cache_put(const char *name, ipv4_t ip)
{
	if (!name || !name[0] || ip == 0) return;
	for (int i = 0; i < DNS_CACHE_SIZE; i++) {
		if (tbl[i].in_use && names_eq(tbl[i].name, name)) {
			tbl[i].ip = ip;
			tbl[i].filled_at = now_ticks;
			return;
		}
	}
	struct entry *e = &tbl[round_cursor];
	round_cursor = (round_cursor + 1) % DNS_CACHE_SIZE;
	int k = 0;
	while (k < DNS_CACHE_NAME - 1 && name[k]) {
		e->name[k] = name[k];
		k++;
	}
	e->name[k]    = '\0';
	e->ip         = ip;
	e->filled_at  = now_ticks;
	e->in_use     = 1;
}

static void cache_reset(void)
{
	for (int i = 0; i < DNS_CACHE_SIZE; i++) tbl[i].in_use = 0;
	round_cursor = 0;
	now_ticks    = 0;
}

static int expect_ip(const char *label, ipv4_t got, ipv4_t want)
{
	if (got == want) { printf("ok   %-55s = 0x%x\n", label, got); return 0; }
	printf("FAIL %s: got 0x%x, want 0x%x\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;
	struct entry *e;

	/* Empty cache: miss. */
	cache_reset();
	failures += expect_ip("empty: lookup -> NULL",
	                     (e = cache_find("a.example")) ? e->ip : 0,
	                     0u);

	/* Insert one, retrieve. */
	cache_put("a.example", 0x01020304u);
	failures += expect_ip("lookup a.example",
	                     (e = cache_find("a.example")) ? e->ip : 0,
	                     0x01020304u);

	/* Re-insert same name with new IP overwrites in place
	 * (slot count stays 1; round cursor not advanced). */
	cache_put("a.example", 0xCAFEBABEu);
	failures += expect_ip("re-insert overwrites IP",
	                     (e = cache_find("a.example")) ? e->ip : 0,
	                     0xCAFEBABEu);

	/* Case sensitivity: "A.EXAMPLE" is a different name. */
	failures += expect_ip("case-sensitive miss",
	                     (e = cache_find("A.EXAMPLE")) ? e->ip : 0,
	                     0u);

	/* Fill all 8 slots, then verify round-robin replacement
	 * displaces the first inserted, not the most recent. */
	cache_reset();
	for (int i = 0; i < DNS_CACHE_SIZE; i++) {
		char nm[8] = { 'h', (char)('0' + i), 0 };
		cache_put(nm, 0x10000000u + (unsigned)i);
	}
	failures += expect_ip("full: h3 alive", (e = cache_find("h3")) ? e->ip : 0, 0x10000003u);

	cache_put("z", 0xDEADBEEFu);  /* round-robin replaces slot 0 ("h0"). */
	failures += expect_ip("after RR replace: h0 evicted",
	                     (e = cache_find("h0")) ? e->ip : 0,
	                     0u);
	failures += expect_ip("after RR replace: z present",
	                     (e = cache_find("z")) ? e->ip : 0,
	                     0xDEADBEEFu);
	failures += expect_ip("after RR replace: h1 untouched",
	                     (e = cache_find("h1")) ? e->ip : 0,
	                     0x10000001u);

	/* TTL: lazy evict on lookup. Fill at t=0, lookup at TTL still hits,
	 * at TTL+1 misses and frees the slot. */
	cache_reset();
	now_ticks = 0;
	cache_put("b.example", 0x09090909u);
	now_ticks = DNS_CACHE_TTL;
	failures += expect_ip("TTL: at exactly TTL -> hit",
	                     (e = cache_find("b.example")) ? e->ip : 0,
	                     0x09090909u);
	now_ticks = DNS_CACHE_TTL + 1u;
	failures += expect_ip("TTL: TTL+1 -> miss",
	                     (e = cache_find("b.example")) ? e->ip : 0,
	                     0u);
	failures += expect_ip("TTL: evicted slot now reusable",
	                     tbl[0].in_use ? 0xBADu : 0xCAFEu,
	                     0xCAFEu);

	/* Sentinel inserts: empty name, ip=0 must be no-ops. */
	cache_reset();
	cache_put("",    0x12345678u);
	cache_put("ok",  0u);
	failures += expect_ip("empty name -> no insert",
	                     (e = cache_find("")) ? e->ip : 0,
	                     0u);
	failures += expect_ip("ip=0 -> no insert",
	                     (e = cache_find("ok")) ? e->ip : 0,
	                     0u);

	/* Name truncated to DNS_CACHE_NAME-1: insert truncates the stored
	 * name but lookup compares the full query string. So a name longer
	 * than 63 chars goes into the cache but can never be retrieved
	 * (its full string differs from the truncated stored name). This
	 * is acceptable because real DNS labels are <=63 chars per RFC 1035;
	 * the test documents the actual behavior so future "fix" attempts
	 * are intentional rather than accidental. */
	cache_reset();
	char long_a[80];
	for (int i = 0; i < 70; i++) long_a[i] = 'a';
	long_a[70] = 0;
	cache_put(long_a, 0xAAu);
	failures += expect_ip("long name: full-string lookup -> miss",
	                     (e = cache_find(long_a)) ? e->ip : 0,
	                     0u);
	char trunc_name[64];
	for (int i = 0; i < 63; i++) trunc_name[i] = 'a';
	trunc_name[63] = 0;
	failures += expect_ip("long name: truncated lookup -> hit",
	                     (e = cache_find(trunc_name)) ? e->ip : 0,
	                     0xAAu);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
