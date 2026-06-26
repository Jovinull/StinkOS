/* Anonymous pipe pool. See pipe.h for the handle encoding and blocking rules. */
#include "defs.h"
#include "pipe.h"

struct pipe {
	unsigned char   buf[PIPE_BUFSZ];
	unsigned int    head;          /* next index to write */
	unsigned int    tail;          /* next index to read */
	unsigned int    count;         /* bytes currently buffered */
	unsigned char   readers;       /* read ends open (0 or 1) */
	unsigned char   writers;       /* write ends open (0 or 1) */
	unsigned char   used;          /* slot in use? */
};

static struct pipe pool[PIPE_MAX];

#define HANDLE_END(h)    ((h) & 1)
#define HANDLE_IDX(h)    ((h) >> 1)
#define MAKE_HANDLE(i,e) (((i) << 1) | (e))
#define END_READ         0
#define END_WRITE        1

static struct pipe *get_pipe(int handle, int expect_end)
{
	if (handle < 0 || handle >= (PIPE_MAX * 2))
		return 0;
	if (HANDLE_END(handle) != expect_end)
		return 0;
	int idx = HANDLE_IDX(handle);
	struct pipe *p = &pool[idx];
	if (!p->used)
		return 0;
	return p;
}

int pipe_alloc(int *rd_handle, int *wr_handle)
{
	if (!rd_handle || !wr_handle)
		return -1;
	for (int i = 0; i < PIPE_MAX; i++) {
		if (pool[i].used)
			continue;
		pool[i].head    = 0;
		pool[i].tail    = 0;
		pool[i].count   = 0;
		pool[i].readers = 1;
		pool[i].writers = 1;
		pool[i].used    = 1;
		*rd_handle = MAKE_HANDLE(i, END_READ);
		*wr_handle = MAKE_HANDLE(i, END_WRITE);
		return 0;
	}
	return -1;
}

int pipe_close(int handle)
{
	if (handle < 0 || handle >= (PIPE_MAX * 2))
		return -1;
	struct pipe *p = &pool[HANDLE_IDX(handle)];
	if (!p->used)
		return -1;
	if (HANDLE_END(handle) == END_READ) {
		if (p->readers)
			p->readers--;
	} else {
		if (p->writers)
			p->writers--;
	}
	if (!p->readers && !p->writers)
		p->used = 0;
	return 0;
}

int pipe_read(int handle, void *dst, unsigned int n)
{
	struct pipe *p = get_pipe(handle, END_READ);
	if (!p)
		return -1;
	if (!dst || n == 0)
		return 0;

	/* Block while empty as long as a writer might still produce bytes. */
	while (p->count == 0 && p->writers > 0)
		__asm__ volatile ("sti; hlt; cli");

	if (p->count == 0)
		return 0;                          /* writers all closed: EOF */

	unsigned char *out = (unsigned char *)dst;
	unsigned int   take = (n < p->count) ? n : p->count;
	for (unsigned int i = 0; i < take; i++) {
		out[i] = p->buf[p->tail];
		p->tail = (p->tail + 1) % PIPE_BUFSZ;
	}
	p->count -= take;
	return (int)take;
}

int pipe_write(int handle, const void *src, unsigned int n)
{
	struct pipe *p = get_pipe(handle, END_WRITE);
	if (!p)
		return -1;
	if (!src || n == 0)
		return 0;

	const unsigned char *in = (const unsigned char *)src;
	unsigned int written = 0;

	while (written < n) {
		/* Block while full as long as a reader might still drain. */
		while (p->count == PIPE_BUFSZ && p->readers > 0)
			__asm__ volatile ("sti; hlt; cli");
		if (p->readers == 0)
			return -1;                 /* EPIPE: no one will ever read */

		unsigned int space = PIPE_BUFSZ - p->count;
		unsigned int chunk = n - written;
		if (chunk > space)
			chunk = space;
		for (unsigned int i = 0; i < chunk; i++) {
			p->buf[p->head] = in[written + i];
			p->head = (p->head + 1) % PIPE_BUFSZ;
		}
		p->count += chunk;
		written  += chunk;
	}
	return (int)written;
}
