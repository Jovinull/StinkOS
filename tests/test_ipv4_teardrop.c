/* Host-side test for the Teardrop / RFC 5722 overlapping-fragment
 * guard in kernel/drivers/net/ipv4.c. The receiver maintains a
 * coverage bitmap (one bit per 8-byte fragment unit). Any incoming
 * fragment that claims a unit already marked covered must drop the
 * whole reassembly slot -- the legitimate sender will retry from
 * scratch, and a Teardrop attacker who is poking at bytes already
 * "written" gets nothing.
 *
 * The slot logic mirrored here keeps the same invariants:
 *   - bitmap entries are added in REASM_UNIT-sized strides
 *   - on overlap, both the bitmap AND the buffer state are cleared
 *   - non-overlapping out-of-order fragments still succeed
 */
#include <stdio.h>
#include <string.h>

#define REASM_MAXLEN 8192u
#define REASM_UNIT   8u
#define REASM_BMAP   ((REASM_MAXLEN / REASM_UNIT + 7u) / 8u)

struct slot {
	int           valid;
	unsigned char buf[REASM_MAXLEN];
	unsigned char bitmap[REASM_BMAP];
	unsigned int  total_len;
};

static void slot_clear(struct slot *s)
{
	s->valid = 0;
	s->total_len = 0;
	memset(s->bitmap, 0, sizeof(s->bitmap));
	memset(s->buf, 0, sizeof(s->buf));
}

static void slot_mark_units(struct slot *s, unsigned int start, unsigned int n)
{
	for (unsigned int i = 0; i < n; i++) {
		unsigned int u = start + i;
		if (u >= REASM_MAXLEN / REASM_UNIT) return;
		s->bitmap[u >> 3] |= (unsigned char)(1u << (u & 7u));
	}
}

/* Returns:
 *   1  fragment accepted
 *   0  rejected because too big
 *  -1  overlap detected, slot reset
 */
static int slot_accept(struct slot *s, unsigned int frag_off,
                       const unsigned char *pld, unsigned int pld_len,
                       int more)
{
	if (frag_off + pld_len > REASM_MAXLEN) return 0;

	unsigned int unit_start = frag_off / REASM_UNIT;
	unsigned int units      = (pld_len + REASM_UNIT - 1u) / REASM_UNIT;

	for (unsigned int u = unit_start; u < unit_start + units; u++) {
		if (u >= REASM_MAXLEN / REASM_UNIT) break;
		if (s->bitmap[u >> 3] & (unsigned char)(1u << (u & 7u))) {
			slot_clear(s);
			return -1;
		}
	}

	for (unsigned int i = 0; i < pld_len; i++)
		s->buf[frag_off + i] = pld[i];

	slot_mark_units(s, unit_start, units);
	s->valid = 1;
	if (!more) s->total_len = frag_off + pld_len;
	return 1;
}

static int expect_int(const char *label, int got, int want)
{
	if (got == want) { printf("ok   %-55s = %d\n", label, got); return 0; }
	printf("FAIL %s: got %d, want %d\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;
	struct slot s; slot_clear(&s);

	unsigned char pld[64];
	for (int i = 0; i < 64; i++) pld[i] = (unsigned char)('A' + (i & 0x1F));

	/* Two non-overlapping fragments at offsets 0 and 64 (8 units each):
	 * both accepted, bitmap covers units 0-15. */
	failures += expect_int("first  fragment off=0, len=64 -> accept",
	                       slot_accept(&s, 0,  pld, 64, 1), 1);
	failures += expect_int("second fragment off=64,len=64 -> accept",
	                       slot_accept(&s, 64, pld, 64, 0), 1);

	/* Now retry frag at offset 0: every unit 0-7 already set => overlap
	 * => the entire slot must reset to zero. */
	failures += expect_int("overlap @ off=0   -> -1 (slot reset)",
	                       slot_accept(&s, 0,  pld, 64, 0), -1);
	failures += expect_int("slot cleared: valid==0",
	                       s.valid,    0);
	failures += expect_int("slot cleared: bitmap[0]==0",
	                       s.bitmap[0], 0);

	/* Partial-overlap attack: legit frag covers [0, 64), attacker sends
	 * [56, 120). Unit 7 (56..63) is shared -- guard fires. */
	slot_clear(&s);
	(void)slot_accept(&s, 0, pld, 64, 1);
	failures += expect_int("partial overlap [56,120) -> -1",
	                       slot_accept(&s, 56, pld, 64, 0), -1);

	/* Out-of-order non-overlap: tail (off=64) before head (off=0) is fine. */
	slot_clear(&s);
	failures += expect_int("OOO tail off=64  -> accept",
	                       slot_accept(&s, 64, pld, 64, 0), 1);
	failures += expect_int("OOO head off=0   -> accept",
	                       slot_accept(&s, 0,  pld, 64, 1), 1);

	/* Too-big rejection: offset+len past REASM_MAXLEN => return 0,
	 * slot left untouched (no reset). */
	slot_clear(&s);
	(void)slot_accept(&s, 0, pld, 64, 1);
	failures += expect_int("over-MAXLEN -> 0 (no overlap reset)",
	                       slot_accept(&s, REASM_MAXLEN - 32, pld, 64, 0), 0);
	failures += expect_int("slot survives oversize reject: valid==1",
	                       s.valid,  1);

	/* Adjacent (not overlapping) at unit boundary: off=64 + len=64
	 * fills units 8-15, then off=128 + len=8 fills unit 16 -- accept. */
	slot_clear(&s);
	(void)slot_accept(&s, 64, pld, 64, 1);
	failures += expect_int("adjacent off=128 len=8 -> accept",
	                       slot_accept(&s, 128, pld, 8, 1), 1);

	/* Re-send identical fragment: same unit_start, same units => overlap. */
	failures += expect_int("re-send identical -> -1",
	                       slot_accept(&s, 128, pld, 8, 1), -1);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
