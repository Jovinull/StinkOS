/* Host-side test for the kernel log ring buffer in
 * kernel/sys/klog.c. Two invariants matter:
 *   - pre-wrap (total < KLOG_SIZE): reads return the last `cap` bytes
 *     in chronological order, ending at the most recent byte
 *   - post-wrap (total == KLOG_SIZE): same, but `start` straddles the
 *     ring's seam; the loop must do `(start + i) % KLOG_SIZE` so the
 *     seam doesn't show up in the output
 *
 * A regression where the seam isn't handled would interleave old and
 * new bytes; one where the start calculation is off by one drops or
 * duplicates the boundary byte. Both are silent in normal operation
 * and only show up after many KiB of log activity.
 */
#include <stdio.h>
#include <string.h>

#define KLOG_SIZE 8192u

static char         ring[KLOG_SIZE];
static unsigned int head;
static unsigned int total;

static void klog_push(char c)
{
	ring[head] = c;
	head = (head + 1u) % KLOG_SIZE;
	if (total < KLOG_SIZE) total++;
}

static unsigned int klog_read(char *out, unsigned int cap)
{
	if (!out || cap == 0 || total == 0) return 0;
	unsigned int avail = total;
	if (avail > cap) avail = cap;
	unsigned int start = (head + KLOG_SIZE - avail) % KLOG_SIZE;
	for (unsigned int i = 0; i < avail; i++)
		out[i] = ring[(start + i) % KLOG_SIZE];
	return avail;
}

static void klog_reset(void)
{
	head = total = 0;
	memset(ring, 0, sizeof(ring));
}

static int expect_u(const char *label, unsigned int got, unsigned int want)
{
	if (got == want) { printf("ok   %-55s = %u\n", label, got); return 0; }
	printf("FAIL %s: got %u, want %u\n", label, got, want);
	return 1;
}

static int expect_str(const char *label, const char *got, unsigned int n,
                      const char *want)
{
	for (unsigned int i = 0; i < n; i++)
		if (got[i] != want[i]) {
			printf("FAIL %s: pos %u got 0x%02x want 0x%02x\n",
			       label, i, (unsigned)(unsigned char)got[i],
			       (unsigned)(unsigned char)want[i]);
			return 1;
		}
	printf("ok   %-55s = (%u bytes)\n", label, n);
	return 0;
}

int main(void)
{
	int failures = 0;
	char buf[KLOG_SIZE + 1];

	/* Empty: read returns 0. */
	klog_reset();
	failures += expect_u("empty -> 0", klog_read(buf, sizeof(buf)), 0);

	/* Push 5 bytes, read all -> 5 in order. */
	for (int i = 0; i < 5; i++) klog_push((char)('a' + i));
	failures += expect_u("after 5 push, total=5", total, 5);
	unsigned int n = klog_read(buf, sizeof(buf));
	failures += expect_u("pre-wrap read 5",      n, 5);
	failures += expect_str("pre-wrap content",   buf, 5, "abcde");

	/* Cap < total: returns last `cap` bytes (here, last 3). */
	n = klog_read(buf, 3);
	failures += expect_u("pre-wrap read cap=3",  n, 3);
	failures += expect_str("pre-wrap content tail", buf, 3, "cde");

	/* Fill exactly KLOG_SIZE: total clamped, head wraps to 0. */
	klog_reset();
	for (unsigned int i = 0; i < KLOG_SIZE; i++)
		klog_push((char)('0' + (i % 10)));
	failures += expect_u("post-fill total == KLOG_SIZE",
	                     total, KLOG_SIZE);
	failures += expect_u("post-fill head wrapped to 0",
	                     head, 0);
	n = klog_read(buf, KLOG_SIZE);
	failures += expect_u("read all post-fill",   n, KLOG_SIZE);
	failures += expect_u("first byte still '0'",   (unsigned)buf[0], '0');
	/* 8192 % 10 = 2, so the last byte cycles to '0'+(8191%10) = '1'. */
	failures += expect_u("last byte cycles to '1'",
	                     (unsigned)buf[KLOG_SIZE - 1u], '1');

	/* Push one more: oldest byte ('0' at position 0) is overwritten;
	 * total stays clamped at KLOG_SIZE, head advances. */
	klog_push('!');
	failures += expect_u("after one overflow: total still KLOG_SIZE",
	                     total, KLOG_SIZE);
	failures += expect_u("head advanced", head, 1u);
	n = klog_read(buf, KLOG_SIZE);
	failures += expect_u("oldest byte now '1', not '0'",
	                     (unsigned)buf[0], '1');
	failures += expect_u("newest byte now '!'",
	                     (unsigned)buf[KLOG_SIZE - 1u], '!');

	/* Wrap by a known amount and check the seam: push 100 fresh bytes
	 * after a full ring; the seam falls 100 bytes in from the end. */
	klog_reset();
	for (unsigned int i = 0; i < KLOG_SIZE; i++) klog_push('X');
	for (int i = 0; i < 100; i++) klog_push((char)('A' + (i % 26)));
	n = klog_read(buf, KLOG_SIZE);
	failures += expect_u("after seam push: read = KLOG_SIZE",
	                     n, KLOG_SIZE);
	/* First (KLOG_SIZE - 100) bytes are still the 'X' tail; last 100
	 * are the fresh A..Z..A.. sequence. */
	failures += expect_u("seam: byte at KLOG_SIZE-100 is 'A'",
	                     (unsigned)buf[KLOG_SIZE - 100u], 'A');
	failures += expect_u("seam: byte just before the seam is 'X'",
	                     (unsigned)buf[KLOG_SIZE - 101u], 'X');
	failures += expect_u("seam: last byte cycles A+99 -> 'V'",
	                     (unsigned)buf[KLOG_SIZE - 1u], (unsigned)('A' + (99 % 26)));

	/* Sanity: NULL or zero-cap reads. */
	failures += expect_u("read NULL out -> 0",      klog_read(0, 100), 0);
	failures += expect_u("read cap=0    -> 0",      klog_read(buf, 0), 0);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
