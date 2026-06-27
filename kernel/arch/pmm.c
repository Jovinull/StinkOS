/* Physical memory manager: a watermark allocator over a physical range, plus a
 * small free-list so released frames can be handed back out. */
#include "pmm.h"
#include "serial.h"

#define FRAME_SIZE   4096u
#define FREE_MAX     1024

static unsigned int start_frame;
static unsigned int next_frame;
static unsigned int end_frame;
static unsigned int free_list[FREE_MAX];
static int free_count;
static int oom_warned;             /* throttle the OOM serial print to once */

void pmm_init(unsigned int start, unsigned int end)
{
	start_frame = (start + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1);
	next_frame = start_frame;
	end_frame = end & ~(FRAME_SIZE - 1);
	free_count = 0;
	oom_warned = 0;
}

unsigned int pmm_alloc(void)
{
	if (free_count > 0)
		return free_list[--free_count];

	if (next_frame + FRAME_SIZE <= end_frame) {
		unsigned int frame = next_frame;
		next_frame += FRAME_SIZE;
		return frame;
	}

	/* Out of physical memory. Print one loud line to serial so a debug
	 * session can spot when OOM hit instead of guessing from a NULL
	 * dereference further up. Throttled so a runaway alloc loop doesn't
	 * drown the log. */
	if (!oom_warned) {
		serial_write("pmm: OUT OF MEMORY (all ");
		serial_write_dec(pmm_total_pages());
		serial_write(" pages handed out)\n");
		oom_warned = 1;
	}
	return 0;
}

unsigned int pmm_total_pages(void)
{
	return (end_frame - start_frame) / FRAME_SIZE;
}

unsigned int pmm_free_pages(void)
{
	unsigned int tail = (end_frame - next_frame) / FRAME_SIZE;
	return tail + (unsigned int)free_count;
}

void pmm_free(unsigned int frame)
{
	frame &= ~(FRAME_SIZE - 1);

	/* Reject the null sentinel and any frame outside the managed range, so a
	 * stray free can never poison the pool or hand back 0 as a real frame. */
	if (frame < start_frame || frame >= end_frame)
		return;

	if (free_count < FREE_MAX)
		free_list[free_count++] = frame;
}
