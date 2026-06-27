/* Host-side test for the TCP out-of-order reassembly slots in
 * kernel/drivers/net/tcp.c (tcb_ooo_park / tcb_ooo_drain). The kernel
 * keeps a 2-slot pool of MSS-sized stashes. The contract:
 *   - park rejects size=0 or > MSS (sender will retransmit)
 *   - re-park of a seq already queued is a no-op (no duplicates)
 *   - park drops when both slots are busy (also a future retransmit)
 *   - drain absorbs every queued seg whose seq matches rcv_nxt,
 *     looping so one absorption can chain another
 *
 * If drain stops looping after one round, an OOO chain like
 * [seq=200] then [seq=100] would leave the 200-slot orphaned until
 * the next packet arrives -- a stall that's hard to spot in QEMU.
 */
#include <stdio.h>
#include <string.h>

#define TCP_MSS 536u

struct ooo_slot {
	int           used;
	unsigned int  seq;
	unsigned int  len;
	unsigned char buf[TCP_MSS];
};

struct tcb_sim {
	struct ooo_slot ooo[2];
	unsigned int    rcv_nxt;
	unsigned char   delivered[8192];
	unsigned int    delivered_len;
};

static void rx_inline(struct tcb_sim *t, const unsigned char *data, unsigned int n)
{
	for (unsigned int i = 0; i < n && t->delivered_len < sizeof(t->delivered); i++)
		t->delivered[t->delivered_len++] = data[i];
	t->rcv_nxt += n;
}

static void park(struct tcb_sim *t, unsigned int seq,
                 const unsigned char *data, unsigned int n)
{
	if (n == 0 || n > TCP_MSS) return;
	for (int k = 0; k < 2; k++)
		if (t->ooo[k].used && t->ooo[k].seq == seq) return;
	for (int k = 0; k < 2; k++) {
		if (t->ooo[k].used) continue;
		t->ooo[k].seq = seq;
		t->ooo[k].len = n;
		for (unsigned int i = 0; i < n; i++) t->ooo[k].buf[i] = data[i];
		t->ooo[k].used = 1;
		return;
	}
}

static void drain(struct tcb_sim *t)
{
	int progress = 1;
	while (progress) {
		progress = 0;
		for (int k = 0; k < 2; k++) {
			if (!t->ooo[k].used) continue;
			if (t->ooo[k].seq != t->rcv_nxt) continue;
			rx_inline(t, t->ooo[k].buf, t->ooo[k].len);
			t->ooo[k].used = 0;
			progress = 1;
		}
	}
}

static int expect_u(const char *label, unsigned int got, unsigned int want)
{
	if (got == want) { printf("ok   %-55s = %u\n", label, got); return 0; }
	printf("FAIL %s: got %u, want %u\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;
	struct tcb_sim t;
	unsigned char payload[1024];
	for (unsigned int i = 0; i < sizeof(payload); i++)
		payload[i] = (unsigned char)('A' + (i & 0x1F));

	/* --- Zero / oversize rejection. ---------------------------------- */
	memset(&t, 0, sizeof(t));
	t.rcv_nxt = 100;
	park(&t, 100, payload, 0);
	failures += expect_u("park n=0 -> no slot used",
	                     t.ooo[0].used + t.ooo[1].used, 0);
	park(&t, 100, payload, TCP_MSS + 1u);
	failures += expect_u("park n>MSS -> no slot used",
	                     t.ooo[0].used + t.ooo[1].used, 0);

	/* --- Single OOO that matches rcv_nxt: drain inlines it. --------- */
	memset(&t, 0, sizeof(t));
	t.rcv_nxt = 100;
	park(&t, 100, payload, 50);
	drain(&t);
	failures += expect_u("park@100 + drain @rcv=100 -> 50 delivered",
	                     t.delivered_len, 50);
	failures += expect_u("post-drain rcv_nxt advanced",
	                     t.rcv_nxt, 150);
	failures += expect_u("post-drain slots free",
	                     t.ooo[0].used + t.ooo[1].used, 0);

	/* --- Chain: park 200 then 100; drain must absorb both in one call. */
	memset(&t, 0, sizeof(t));
	t.rcv_nxt = 100;
	park(&t, 200, payload + 100, 50);   /* future hole */
	park(&t, 100, payload,         100); /* fills the gap */
	drain(&t);
	failures += expect_u("chain: both segs absorbed",
	                     t.delivered_len, 150);
	failures += expect_u("chain: rcv_nxt jumped to 250",
	                     t.rcv_nxt, 250);
	failures += expect_u("chain: slots free",
	                     t.ooo[0].used + t.ooo[1].used, 0);

	/* --- Park-of-same-seq is no-op (no duplicate). ----------------- */
	memset(&t, 0, sizeof(t));
	t.rcv_nxt = 100;
	park(&t, 200, payload, 50);
	park(&t, 200, payload, 50);
	failures += expect_u("re-park same seq -> still 1 slot",
	                     t.ooo[0].used + t.ooo[1].used, 1);

	/* --- Park overflow: third disjoint seq is dropped. ------------- */
	memset(&t, 0, sizeof(t));
	t.rcv_nxt = 100;
	park(&t, 200, payload, 50);
	park(&t, 300, payload, 50);
	park(&t, 400, payload, 50);
	failures += expect_u("overflow: 2 slots used, 3rd dropped",
	                     t.ooo[0].used + t.ooo[1].used, 2);

	/* --- OOO that never lines up: stays parked, never delivered. --- */
	memset(&t, 0, sizeof(t));
	t.rcv_nxt = 100;
	park(&t, 200, payload, 50);
	drain(&t);
	failures += expect_u("stuck @200 stays parked",
	                     t.ooo[0].used + t.ooo[1].used, 1);
	failures += expect_u("stuck: nothing delivered",
	                     t.delivered_len, 0);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
