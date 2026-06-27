/* Host-side test for the anonymous-pipe pool in
 * kernel/sys/pipe.c. The blocking primitive (`sti; hlt; cli`) lives
 * only in kernel mode so we can't drive it from userspace, but every
 * other invariant -- handle encoding, ring-buffer wrap, EOF on
 * writers=0, EPIPE on readers=0, slot recycling on dual-close -- is
 * pure data flow and is what this test pins.
 */
#include <stdio.h>

#define PIPE_MAX     8
#define PIPE_BUFSZ   4096

struct pipe {
	unsigned char buf[PIPE_BUFSZ];
	unsigned int  head, tail, count;
	unsigned char readers, writers, used;
};

static struct pipe pool[PIPE_MAX];

#define HANDLE_END(h)    ((h) & 1)
#define HANDLE_IDX(h)    ((h) >> 1)
#define MAKE_HANDLE(i,e) (((i) << 1) | (e))
#define END_READ         0
#define END_WRITE        1

static struct pipe *get_pipe(int handle, int expect_end)
{
	if (handle < 0 || handle >= (PIPE_MAX * 2)) return 0;
	if (HANDLE_END(handle) != expect_end) return 0;
	int idx = HANDLE_IDX(handle);
	struct pipe *p = &pool[idx];
	if (!p->used) return 0;
	return p;
}

static int pipe_alloc(int *rd, int *wr)
{
	if (!rd || !wr) return -1;
	for (int i = 0; i < PIPE_MAX; i++) {
		if (pool[i].used) continue;
		pool[i].head = pool[i].tail = pool[i].count = 0;
		pool[i].readers = pool[i].writers = 1;
		pool[i].used = 1;
		*rd = MAKE_HANDLE(i, END_READ);
		*wr = MAKE_HANDLE(i, END_WRITE);
		return 0;
	}
	return -1;
}

static int pipe_close(int handle)
{
	if (handle < 0 || handle >= (PIPE_MAX * 2)) return -1;
	struct pipe *p = &pool[HANDLE_IDX(handle)];
	if (!p->used) return -1;
	if (HANDLE_END(handle) == END_READ) {
		if (p->readers) p->readers--;
	} else {
		if (p->writers) p->writers--;
	}
	if (!p->readers && !p->writers) p->used = 0;
	return 0;
}

/* Non-blocking variants: skip the busy-wait loop. The actual kernel
 * would block; the body that runs after the loop is identical. */
static int pipe_read_nb(int handle, void *dst, unsigned int n)
{
	struct pipe *p = get_pipe(handle, END_READ);
	if (!p) return -1;
	if (!dst || n == 0) return 0;
	if (p->count == 0 && p->writers == 0) return 0;     /* EOF */
	if (p->count == 0) return -2;                       /* would block */
	unsigned char *out = (unsigned char *)dst;
	unsigned int take = (n < p->count) ? n : p->count;
	for (unsigned int i = 0; i < take; i++) {
		out[i] = p->buf[p->tail];
		p->tail = (p->tail + 1) % PIPE_BUFSZ;
	}
	p->count -= take;
	return (int)take;
}

static int pipe_write_nb(int handle, const void *src, unsigned int n)
{
	struct pipe *p = get_pipe(handle, END_WRITE);
	if (!p) return -1;
	if (!src || n == 0) return 0;
	if (p->readers == 0) return -1;                     /* EPIPE */
	if (p->count == PIPE_BUFSZ) return -2;              /* would block */
	const unsigned char *in = (const unsigned char *)src;
	unsigned int space = PIPE_BUFSZ - p->count;
	unsigned int chunk = (n < space) ? n : space;
	for (unsigned int i = 0; i < chunk; i++) {
		p->buf[p->head] = in[i];
		p->head = (p->head + 1) % PIPE_BUFSZ;
	}
	p->count += chunk;
	return (int)chunk;
}

static void pool_reset(void)
{
	for (int i = 0; i < PIPE_MAX; i++) pool[i].used = 0;
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
	int rd, wr;
	char buf[16];

	/* --- alloc / handle encoding ----------------------------------- */
	pool_reset();
	failures += expect_int("alloc 1 -> 0", pipe_alloc(&rd, &wr), 0);
	failures += expect_int("rd is END_READ",  HANDLE_END(rd),  END_READ);
	failures += expect_int("wr is END_WRITE", HANDLE_END(wr),  END_WRITE);
	failures += expect_int("rd idx == wr idx", HANDLE_IDX(rd), HANDLE_IDX(wr));

	/* --- end mismatch is a permission/handle error ----------------- */
	failures += expect_int("read on wr handle -> -1",
	                       pipe_read_nb(wr, buf, sizeof(buf)), -1);
	failures += expect_int("write on rd handle -> -1",
	                       pipe_write_nb(rd, "x", 1), -1);

	/* --- simple write / read --------------------------------------- */
	failures += expect_int("write 5 -> 5",
	                       pipe_write_nb(wr, "hello", 5), 5);
	failures += expect_int("read 5 -> 5",
	                       pipe_read_nb(rd, buf, 5), 5);
	failures += expect_int("read byte[0]=='h'", buf[0], 'h');
	failures += expect_int("read byte[4]=='o'", buf[4], 'o');

	/* --- ring wrap: write/read close to BUFSZ -------------------- */
	/* Force head/tail to wrap by writing & draining repeatedly. */
	pool_reset();
	(void)pipe_alloc(&rd, &wr);
	for (int i = 0; i < 5; i++) {
		failures += expect_int("ring: write 1000",
		                       pipe_write_nb(wr, "0123456789012345", 16), 16);
		failures += expect_int("ring: read 1000",
		                       pipe_read_nb(rd, buf, 16), 16);
	}
	/* After 5 cycles of 16 bytes, head and tail both at (5*16)%BUFSZ = 80. */
	failures += expect_int("ring: head == 80", (int)pool[0].head, 80);
	failures += expect_int("ring: tail == 80", (int)pool[0].tail, 80);

	/* --- write more than space available: takes only space bytes -- */
	pool_reset();
	(void)pipe_alloc(&rd, &wr);
	pool[0].count = PIPE_BUFSZ - 4;          /* simulate near-full */
	pool[0].head  = PIPE_BUFSZ - 4;
	failures += expect_int("write 16 when space=4 -> 4",
	                       pipe_write_nb(wr, "ABCDEFGHIJKLMNOP", 16), 4);
	failures += expect_int("after partial write count=BUFSZ",
	                       (int)pool[0].count, PIPE_BUFSZ);
	failures += expect_int("would-block on full -> -2",
	                       pipe_write_nb(wr, "x", 1), -2);

	/* --- read past available: take = count ------------------------ */
	pool_reset();
	(void)pipe_alloc(&rd, &wr);
	(void)pipe_write_nb(wr, "abc", 3);
	failures += expect_int("read 16 when count=3 -> 3",
	                       pipe_read_nb(rd, buf, 16), 3);

	/* --- EOF: writer closed, drain remaining, then 0 -------------- */
	pool_reset();
	(void)pipe_alloc(&rd, &wr);
	(void)pipe_write_nb(wr, "x", 1);
	(void)pipe_close(wr);
	failures += expect_int("EOF: read drains last byte -> 1",
	                       pipe_read_nb(rd, buf, 16), 1);
	failures += expect_int("EOF: subsequent read -> 0",
	                       pipe_read_nb(rd, buf, 16), 0);

	/* --- EPIPE: reader closed, writer gets -1 --------------------- */
	pool_reset();
	(void)pipe_alloc(&rd, &wr);
	(void)pipe_close(rd);
	failures += expect_int("EPIPE: write after reader close -> -1",
	                       pipe_write_nb(wr, "x", 1), -1);

	/* --- slot recycled after both ends closed --------------------- */
	pool_reset();
	(void)pipe_alloc(&rd, &wr);
	failures += expect_int("slot 0 used after alloc", (int)pool[0].used, 1);
	(void)pipe_close(rd);
	failures += expect_int("slot 0 still used (one end open)",
	                       (int)pool[0].used, 1);
	(void)pipe_close(wr);
	failures += expect_int("slot 0 freed after both ends close",
	                       (int)pool[0].used, 0);

	/* --- alloc exhaustion ----------------------------------------- */
	pool_reset();
	int rds[PIPE_MAX], wrs[PIPE_MAX];
	for (int i = 0; i < PIPE_MAX; i++)
		(void)pipe_alloc(&rds[i], &wrs[i]);
	failures += expect_int("alloc 9th when MAX=8 -> -1",
	                       pipe_alloc(&rd, &wr), -1);

	/* Close one and re-alloc: succeeds. */
	(void)pipe_close(rds[3]);
	(void)pipe_close(wrs[3]);
	failures += expect_int("re-alloc after dual-close -> 0",
	                       pipe_alloc(&rd, &wr), 0);

	/* --- handle out-of-range rejected ----------------------------- */
	failures += expect_int("invalid handle -1 -> read -1",
	                       pipe_read_nb(-1, buf, 1), -1);
	failures += expect_int("invalid handle BIG -> write -1",
	                       pipe_write_nb(PIPE_MAX * 2, "x", 1), -1);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
