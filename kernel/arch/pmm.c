/* Physical memory manager: a watermark allocator over a physical range, plus a
 * small free-list so released frames can be handed back out.
 *
 * Per-frame refcount underpins COW fork (v0.7): a single frame can be shared
 * across multiple pgdirs after `pmm_ref_inc`; `pmm_free` then decrements
 * and only returns the frame to the pool when the last owner drops it. */
#include "pmm.h"
#include "serial.h"

#define FRAME_SIZE   4096u
#define FREE_MAX     1024
/* Sized to cover the PMM upper bound (32 MiB today; main.c clamps `end` at
 * 0x2000000). 32 MiB / 4 KiB = 8192 possible frames in the managed range. */
#define MAX_FRAMES   8192

static unsigned int start_frame;
static unsigned int next_frame;
static unsigned int end_frame;
static unsigned int free_list[FREE_MAX];
static int free_count;
static int oom_warned;             /* throttle the OOM serial print to once */
/* refcount[idx] where idx = (frame - start_frame) / FRAME_SIZE. u8 saturates
 * at 255; with 8192 frames + max share count = process count, never close. */
static unsigned char refcount[MAX_FRAMES];

static int in_range(unsigned int frame)
{
	return frame >= start_frame && frame < end_frame;
}

static unsigned int frame_idx(unsigned int frame)
{
	return (frame - start_frame) / FRAME_SIZE;
}

void pmm_init(unsigned int start, unsigned int end)
{
	start_frame = (start + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1);
	next_frame = start_frame;
	end_frame = end & ~(FRAME_SIZE - 1);
	free_count = 0;
	oom_warned = 0;
	for (unsigned int i = 0; i < MAX_FRAMES; i++)
		refcount[i] = 0;
}

unsigned int pmm_alloc(void)
{
	unsigned int frame = 0;
	if (free_count > 0) {
		frame = free_list[--free_count];
	} else if (next_frame + FRAME_SIZE <= end_frame) {
		frame = next_frame;
		next_frame += FRAME_SIZE;
	} else {
		/* Out of physical memory. Print one loud line to serial so a
		 * debug session can spot when OOM hit instead of guessing from
		 * a NULL dereference further up. Throttled so a runaway alloc
		 * loop doesn't drown the log. */
		if (!oom_warned) {
			serial_write("pmm: OUT OF MEMORY (all ");
			serial_write_dec(pmm_total_pages());
			serial_write(" pages handed out)\n");
			oom_warned = 1;
		}
		return 0;
	}
	/* Fresh handout: refcount must be 0 here (we either dequeued a
	 * freed frame or bumped the watermark over virgin memory). Force
	 * to 1 either way so a stale entry can't trick COW into freeing
	 * a still-active frame. */
	refcount[frame_idx(frame)] = 1;
	return frame;
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
	if (!in_range(frame))
		return;

	unsigned int idx = frame_idx(frame);
	if (refcount[idx] == 0)
		return;                            /* double free / never allocated */
	if (--refcount[idx] > 0)
		return;                            /* still has owners (COW share) */

	if (free_count < FREE_MAX)
		free_list[free_count++] = frame;
}

void pmm_ref_inc(unsigned int frame)
{
	frame &= ~(FRAME_SIZE - 1);
	if (!in_range(frame))
		return;
	unsigned int idx = frame_idx(frame);
	if (refcount[idx] < 255u)
		refcount[idx]++;
}

unsigned int pmm_ref(unsigned int frame)
{
	frame &= ~(FRAME_SIZE - 1);
	if (!in_range(frame))
		return 0;
	return refcount[frame_idx(frame)];
}
