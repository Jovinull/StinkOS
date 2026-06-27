/* Host-side test for the fb_rect pre-loop clip in
 * kernel/drivers/video/fb.c. The kernel clips x0/y0 against
 * (width, height) and shrinks w/h to fit BEFORE entering the per-
 * pixel loops -- without this gate a userland call with w = h =
 * 0xFFFFFFFF would spin the kernel through ~1e19 fb_putpixel calls
 * even though every single one would be rejected.
 *
 * We mirror the clip arithmetic and check the (w, h) the loop body
 * actually iterates over -- the property that gates kernel CPU time.
 */
#include <stdio.h>

static unsigned int width  = 1024;
static unsigned int height = 768;

struct clip { int reject; unsigned int w; unsigned int h; };

static struct clip fb_rect_clip(unsigned int x0, unsigned int y0,
                                unsigned int w,  unsigned int h)
{
	struct clip c = {0, 0, 0};
	if (x0 >= width || y0 >= height) { c.reject = 1; return c; }
	if (w > width  - x0) w = width  - x0;
	if (h > height - y0) h = height - y0;
	c.w = w; c.h = h;
	return c;
}

static int expect_int(const char *label, int got, int want)
{
	if (got == want) { printf("ok   %-55s = %d\n", label, got); return 0; }
	printf("FAIL %s: got %d, want %d\n", label, got, want);
	return 1;
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
	struct clip c;

	/* Inside-bounds rect: passes through verbatim. */
	c = fb_rect_clip(10, 20, 30, 40);
	failures += expect_int("inside: reject==0",           c.reject, 0);
	failures += expect_u  ("inside: w == 30",             c.w,      30);
	failures += expect_u  ("inside: h == 40",             c.h,      40);

	/* Rect at the very edge: w/h shrunk to fit. */
	c = fb_rect_clip(1020, 760, 100, 100);
	failures += expect_int("edge: reject==0",             c.reject, 0);
	failures += expect_u  ("edge: w clipped to 4",        c.w,      4);
	failures += expect_u  ("edge: h clipped to 8",        c.h,      8);

	/* x0 at the wall: w == 0 (loop runs zero times). */
	c = fb_rect_clip(width - 1, 0, 5, 5);
	failures += expect_int("x0=width-1: reject==0",       c.reject, 0);
	failures += expect_u  ("x0=width-1: w == 1",          c.w,      1);

	/* x0 past the wall: full reject. */
	c = fb_rect_clip(width, 0, 5, 5);
	failures += expect_int("x0=width: reject==1",         c.reject, 1);

	/* y0 past the wall: full reject. */
	c = fb_rect_clip(0, height, 5, 5);
	failures += expect_int("y0=height: reject==1",        c.reject, 1);

	/* Pathological all-FF: this is the bug the clip prevents. The
	 * unclipped loop would iterate ~1e19 times; the clipped one runs
	 * at most width * height = ~786K. */
	c = fb_rect_clip(0, 0, 0xFFFFFFFFu, 0xFFFFFFFFu);
	failures += expect_int("all-FF: reject==0",           c.reject, 0);
	failures += expect_u  ("all-FF: w clipped to width",  c.w,      width);
	failures += expect_u  ("all-FF: h clipped to height", c.h,      height);

	/* x0 wraparound attempt: width - x0 underflows if x0 > width, but
	 * the early reject fires first -- we exit before the subtraction. */
	c = fb_rect_clip(0xFFFFFF00u, 0, 1, 1);
	failures += expect_int("huge x0: reject==1",          c.reject, 1);

	/* Zero-sized rect: legitimate no-op, no reject. */
	c = fb_rect_clip(0, 0, 0, 0);
	failures += expect_int("zero rect: reject==0",        c.reject, 0);
	failures += expect_u  ("zero rect: w == 0",           c.w,      0);
	failures += expect_u  ("zero rect: h == 0",           c.h,      0);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
