/* Kernel log ring buffer -- see klog.h for the contract. */
#include "klog.h"

static char         ring[KLOG_SIZE];
static unsigned int head;          /* next byte to write */
static unsigned int total;         /* bytes ever written (clamped at KLOG_SIZE) */

void klog_push(char c)
{
	ring[head] = c;
	head = (head + 1u) % KLOG_SIZE;
	if (total < KLOG_SIZE)
		total++;
}

unsigned int klog_read(char *out, unsigned int cap)
{
	if (!out || cap == 0 || total == 0)
		return 0;

	unsigned int avail = total;
	if (avail > cap)
		avail = cap;

	/* When total < KLOG_SIZE the ring hasn't wrapped, so bytes 0..head-1
	 * are in order. When it has wrapped, the oldest byte sits at `head`
	 * and the newest one byte before it. Walk backwards `avail` slots
	 * from head to find the start. */
	unsigned int start = (head + KLOG_SIZE - avail) % KLOG_SIZE;
	for (unsigned int i = 0; i < avail; i++)
		out[i] = ring[(start + i) % KLOG_SIZE];
	return avail;
}
