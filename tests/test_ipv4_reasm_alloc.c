/* Host-side test for the 2-slot fragment reassembly pool allocator
 * in kernel/drivers/net/ipv4.c (reasm_find_or_alloc). Contract:
 *   - exact match (src, dst, id, proto) returns existing slot and
 *     refreshes its last_tick
 *   - otherwise the first free slot wins
 *   - if both slots are in use, any slot older than REASM_TTL is
 *     evicted and reused; if none are stale, allocation fails
 *
 * A regression where the allocator returns the same slot for two
 * concurrent flows would corrupt both reassemblies silently --
 * exactly the kind of thing two test IPs in QEMU never hit.
 */
#include <stdio.h>

#define REASM_SLOTS 2
#define REASM_TTL   3000u
#define REASM_BMAP  128

typedef unsigned int ipv4_t;

struct slot {
	int          in_use;
	ipv4_t       src_ip, dst_ip;
	unsigned short ip_id;
	unsigned char  protocol;
	unsigned int   last_tick;
	unsigned char  bitmap[REASM_BMAP];
	unsigned int   total_len;
};

static struct slot pool[REASM_SLOTS];
static unsigned int now_ticks;

static void slot_clear(struct slot *r)
{
	r->in_use    = 0;
	r->total_len = 0;
	r->last_tick = 0;
	for (int i = 0; i < REASM_BMAP; i++) r->bitmap[i] = 0;
}

static struct slot *find_or_alloc(ipv4_t src, ipv4_t dst,
                                  unsigned short id, unsigned char proto)
{
	int free_idx = -1, stale_idx = -1;
	for (int i = 0; i < REASM_SLOTS; i++) {
		struct slot *r = &pool[i];
		if (r->in_use && r->src_ip == src && r->dst_ip == dst &&
		    r->ip_id == id && r->protocol == proto) {
			r->last_tick = now_ticks;
			return r;
		}
		if (!r->in_use && free_idx < 0)
			free_idx = i;
		else if (r->in_use &&
		         (unsigned int)(now_ticks - r->last_tick) > REASM_TTL)
			stale_idx = i;
	}
	int pick = (free_idx >= 0) ? free_idx : stale_idx;
	if (pick < 0) return 0;
	struct slot *r = &pool[pick];
	slot_clear(r);
	r->in_use   = 1;
	r->src_ip   = src;
	r->dst_ip   = dst;
	r->ip_id    = id;
	r->protocol = proto;
	r->last_tick = now_ticks;
	return r;
}

static void pool_reset(void)
{
	for (int i = 0; i < REASM_SLOTS; i++) slot_clear(&pool[i]);
	now_ticks = 0;
}

static int expect_int(const char *label, int got, int want)
{
	if (got == want) { printf("ok   %-55s = %d\n", label, got); return 0; }
	printf("FAIL %s: got %d, want %d\n", label, got, want);
	return 1;
}

static int slot_idx(struct slot *r)
{
	if (!r) return -1;
	return (int)(r - pool);
}

int main(void)
{
	int failures = 0;
	struct slot *a, *b, *c;

	/* Empty pool: first alloc lands in slot 0. */
	pool_reset();
	a = find_or_alloc(1, 2, 100, 17);
	failures += expect_int("first alloc -> slot 0", slot_idx(a), 0);

	/* Second flow: lands in slot 1. */
	b = find_or_alloc(1, 3, 200, 17);
	failures += expect_int("second flow -> slot 1", slot_idx(b), 1);

	/* Same-key lookup of first flow: hits slot 0 again. */
	a = find_or_alloc(1, 2, 100, 17);
	failures += expect_int("re-lookup flow A -> slot 0", slot_idx(a), 0);

	/* Lookup refreshes last_tick. */
	now_ticks = 1000;
	a = find_or_alloc(1, 2, 100, 17);
	failures += expect_int("flow A last_tick refreshed",
	                       (int)pool[0].last_tick, 1000);

	/* All slots full, fresh flow with no stale survivors: allocation
	 * fails (returns NULL). */
	now_ticks = 1100;
	c = find_or_alloc(9, 9, 999, 6);
	failures += expect_int("full pool, no stale -> NULL", slot_idx(c), -1);

	/* Make slot 1 stale, retry: it gets evicted and reused. */
	now_ticks = 1000 + REASM_TTL + 1;       /* slot 1 last_tick=1, idle > TTL */
	c = find_or_alloc(9, 9, 999, 6);
	failures += expect_int("stale -> reuse slot 1", slot_idx(c), 1);

	/* Different protocol with same 4-tuple is a different flow. */
	pool_reset();
	a = find_or_alloc(1, 2, 100, 6);
	b = find_or_alloc(1, 2, 100, 17);
	failures += expect_int("same 4-tuple diff proto: two slots",
	                       slot_idx(a) + slot_idx(b), 0 + 1);

	/* Different IP id is also a different flow (same 5-tuple proto). */
	pool_reset();
	a = find_or_alloc(1, 2, 100, 6);
	b = find_or_alloc(1, 2, 101, 6);
	failures += expect_int("same key diff id: two slots",
	                       slot_idx(a) + slot_idx(b), 0 + 1);

	/* Tick wraparound: a stale slot whose last_tick was just below
	 * 0xFFFFFFFF must still expire correctly under unsigned subtract. */
	pool_reset();
	now_ticks = 0xFFFFFFFFu - 100u;
	a = find_or_alloc(1, 2, 100, 6);     /* slot 0 stamped near rollover */
	b = find_or_alloc(1, 3, 200, 6);     /* slot 1 */
	now_ticks = 0xFFFFFFFFu - 100u + REASM_TTL + 1u;   /* both stale */
	c = find_or_alloc(9, 9, 999, 6);
	failures += expect_int("wraparound stale: returns valid slot",
	                       c != 0, 1);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
