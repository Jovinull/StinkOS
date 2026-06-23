/* Physical memory manager: a watermark allocator over a physical range, plus a
 * small free-list so released frames can be handed back out. */
#include "pmm.h"

#define FRAME_SIZE   4096u
#define FREE_MAX     1024

static unsigned int start_frame;
static unsigned int next_frame;
static unsigned int end_frame;
static unsigned int free_list[FREE_MAX];
static int free_count;

void pmm_init(unsigned int start, unsigned int end)
{
	start_frame = (start + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1);
	next_frame = start_frame;
	end_frame = end & ~(FRAME_SIZE - 1);
	free_count = 0;
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
	return 0;                       /* out of physical memory */
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
