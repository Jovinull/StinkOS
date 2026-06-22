/* Physical memory manager: a watermark allocator over a physical range, plus a
 * small free-list so released frames can be handed back out. */
#include "pmm.h"

#define FRAME_SIZE   4096u
#define FREE_MAX     1024

static unsigned int next_frame;
static unsigned int end_frame;
static unsigned int free_list[FREE_MAX];
static int free_count;

void pmm_init(unsigned int start, unsigned int end)
{
	next_frame = (start + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1);
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
	if (free_count < FREE_MAX)
		free_list[free_count++] = frame & ~(FRAME_SIZE - 1);
}
