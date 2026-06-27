/* Host-side test for the tcb_rx_wnd helper that now drives the TCP
 * advertised window. Before this, every outgoing segment claimed
 * TCP_BUFFER_SIZE bytes of receive room regardless of how much was
 * already pending unread in rx_buf -- a fast peer would happily overrun
 * the ring and the kernel would silently drop data without slowing it
 * down via flow control.
 *
 * Definition (sliding-ring with sentinel): free = SIZE - 1 - used,
 * where used = (tail - head) mod SIZE. The -1 is the canonical
 * "head == tail means empty" sentinel slot; without it tail+1==head
 * would look like "full" and "empty" simultaneously.
 */
#include <stdio.h>

#define TCP_BUFFER_SIZE 4096

static unsigned int tcb_rx_wnd(unsigned int head, unsigned int tail)
{
	unsigned int used = (tail + TCP_BUFFER_SIZE - head) % TCP_BUFFER_SIZE;
	if (used + 1 >= TCP_BUFFER_SIZE) return 0;
	return TCP_BUFFER_SIZE - 1u - used;
}

static int expect_uint(const char *label, unsigned int got, unsigned int want)
{
	if (got == want) { printf("ok   %-50s = %u\n", label, got); return 0; }
	printf("FAIL %s: got %u, want %u\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;

	/* Empty ring: max available room (minus 1 for the sentinel). */
	failures += expect_uint("empty: wnd = SIZE - 1",      tcb_rx_wnd(0, 0),         4095);
	failures += expect_uint("empty at mid: wnd = SIZE-1", tcb_rx_wnd(1000, 1000),   4095);

	/* One byte in flight. */
	failures += expect_uint("1 byte pending: wnd = 4094", tcb_rx_wnd(0, 1),         4094);

	/* Half full. */
	failures += expect_uint("half full: wnd = 2047",      tcb_rx_wnd(0, 2048),      2047);

	/* Almost full: only one byte free. */
	failures += expect_uint("near full: wnd = 1",         tcb_rx_wnd(0, 4094),      1);

	/* Sentinel-full state: tail+1 == head -> wnd = 0. */
	failures += expect_uint("sentinel full: wnd = 0",     tcb_rx_wnd(0, 4095),      0);
	failures += expect_uint("sentinel full mid: wnd = 0", tcb_rx_wnd(100, 99),      0);

	/* Wrapped: tail < head. */
	failures += expect_uint("wrapped half full: wnd",     tcb_rx_wnd(2048, 0),      2047);
	/* tail=1, head=2 puts used at exactly SIZE-1 = sentinel-full. */
	failures += expect_uint("wrapped sentinel full",      tcb_rx_wnd(2, 1),         0);
	failures += expect_uint("wrapped 1 byte free",        tcb_rx_wnd(3, 1),         1);

	/* After draining: head advances, free space grows. */
	failures += expect_uint("drain 100 of 4094: wnd = 101", tcb_rx_wnd(100, 4094),  101);

	/* Sanity: wnd + used + 1 == SIZE for any non-full state. */
	for (unsigned int used = 0; used < 4095; used++) {
		unsigned int wnd = tcb_rx_wnd(0, used);
		if (wnd + used + 1 != 4096) {
			printf("FAIL invariant at used=%u: wnd=%u\n", used, wnd);
			failures++;
		}
	}
	if (failures == 0)
		printf("ok   invariant: wnd + used + 1 == SIZE for all non-full\n");

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
