/* Host test for lib/libstink_surface.c -- the real file, not a model.
 *
 * Contract under test: no primitive ever writes outside px[0 .. w*h),
 * whatever coordinates and sizes it is handed, and what it draws inside
 * is exactly the clipped shape.
 *
 * Every surface sits between two guard bands filled with a sentinel. The
 * primitives are driven with coordinates from an adversarial table
 * (INT_MIN, INT_MAX, off-by-one around every edge, values whose sum
 * overflows) in every combination, and the guards are checked after each
 * call. For fills and blits the number and position of changed pixels is
 * compared against an independent 64-bit computation of the clipped
 * rectangle.
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libstink_surface.h"

#define W      37
#define H      23
#define GUARD  4096
#define SENT   0xDEADBEEFu
#define BG     0x11111111u

static unsigned int store[GUARD + W * H + GUARD];
static struct stk_surface surf = { store + GUARD, W, H };
static int failures;
static long calls;

static void reset(void)
{
	for (int i = 0; i < GUARD; i++) {
		store[i] = SENT;
		store[GUARD + W * H + i] = SENT;
	}
	for (int i = 0; i < W * H; i++)
		surf.px[i] = BG;
}

static int guards_ok(void)
{
	for (int i = 0; i < GUARD; i++)
		if (store[i] != SENT || store[GUARD + W * H + i] != SENT)
			return 0;
	return 1;
}

static void fail(const char *what, int a, int b, int c, int d)
{
	if (failures < 20)
		printf("FAIL %s (%d, %d, %d, %d)\n", what, a, b, c, d);
	failures++;
}

static const int EDGE[] = {
	INT_MIN, INT_MIN + 1, -1000000, -W - 1, -W, -H, -17, -2, -1, 0, 1, 2, 5,
	H - 1, H, H + 1, W - 1, W, W + 1, 1000000, INT_MAX - 1, INT_MAX,
};
#define NEDGE ((int)(sizeof(EDGE) / sizeof(EDGE[0])))

/* Reference clip of [pos, pos+len) to [0, limit) in 64-bit. */
static void ref_span(int pos, int len, int limit, long long *a, long long *b)
{
	*a = pos;
	*b = (long long)pos + len;
	if (len <= 0) { *a = *b = 0; return; }
	if (*a < 0) *a = 0;
	if (*b > limit) *b = limit;
	if (*b < *a) *b = *a;
}

static void test_fill(void)
{
	for (int xi = 0; xi < NEDGE; xi++)
	for (int yi = 0; yi < NEDGE; yi++)
	for (int wi = 0; wi < NEDGE; wi++)
	for (int hi = 0; hi < NEDGE; hi++) {
		int x = EDGE[xi], y = EDGE[yi], w = EDGE[wi], h = EDGE[hi];
		reset();
		surf_fill(&surf, x, y, w, h, 0x00ABCDEFu);
		calls++;
		if (!guards_ok()) { fail("fill wrote outside the surface", x, y, w, h); continue; }
		long long x0, x1, y0, y1;
		ref_span(x, w, W, &x0, &x1);
		ref_span(y, h, H, &y0, &y1);
		int bad = 0;
		for (int py = 0; py < H && !bad; py++)
			for (int px = 0; px < W; px++) {
				int in = px >= x0 && px < x1 && py >= y0 && py < y1;
				unsigned int want = in ? 0x00ABCDEFu : BG;
				if (surf.px[py * W + px] != want) { bad = 1; break; }
			}
		if (bad) fail("fill drew the wrong rectangle", x, y, w, h);
	}
}

static void test_rect_line_pixel(void)
{
	for (int xi = 0; xi < NEDGE; xi++)
	for (int yi = 0; yi < NEDGE; yi++)
	for (int wi = 0; wi < NEDGE; wi++)
	for (int hi = 0; hi < NEDGE; hi++) {
		int a = EDGE[xi], b = EDGE[yi], c = EDGE[wi], d = EDGE[hi];
		reset();
		surf_rect(&surf, a, b, c, d, 0xFFFFFF);
		surf_line(&surf, a, b, c, d, 0xFFFFFF);
		surf_pixel(&surf, a, b, 0xFFFFFF);
		surf_pixel(&surf, c, d, 0xFFFFFF);
		calls += 4;
		if (!guards_ok()) fail("rect/line/pixel wrote outside the surface", a, b, c, d);
	}

	/* A line between two in-bounds points touches both ends. */
	reset();
	surf_line(&surf, 2, 3, W - 2, H - 2, 0x00FF00);
	if (surf.px[3 * W + 2] != 0x00FF00 || surf.px[(H - 2) * W + (W - 2)] != 0x00FF00)
		fail("line missed an endpoint", 2, 3, W - 2, H - 2);
	/* A line crossing the whole surface from far outside still draws. */
	reset();
	surf_line(&surf, -5000, H / 2, 5000, H / 2, 0x0000FF);
	int lit = 0;
	for (int px = 0; px < W; px++)
		if (surf.px[(H / 2) * W + px] == 0x0000FF)
			lit++;
	if (lit != W) fail("clipped horizontal line is incomplete", lit, W, 0, 0);
	/* The outline of an in-bounds rectangle is exactly its perimeter. */
	reset();
	surf_rect(&surf, 4, 5, 10, 6, 0xFF0000);
	int per = 0;
	for (int i = 0; i < W * H; i++)
		if (surf.px[i] == 0xFF0000)
			per++;
	if (per != 2 * 10 + 2 * 6 - 4) fail("rect outline pixel count", per, 0, 0, 0);
}

static void test_text(void)
{
	char longstr[600];
	for (int i = 0; i < 599; i++)
		longstr[i] = (char)(32 + i % 95);
	longstr[599] = 0;
	const char weird[] = { 'A', (char)0x01, (char)0x7F, (char)0xFF, 'z', 0 };

	for (int xi = 0; xi < NEDGE; xi++)
	for (int yi = 0; yi < NEDGE; yi++) {
		int x = EDGE[xi], y = EDGE[yi];
		reset();
		surf_text(&surf, x, y, longstr, 0xFFFFFF);
		surf_text(&surf, x, y, weird, 0xFFFFFF);
		surf_text_n(&surf, x, y, longstr, 3, 0xFFFFFF);
		surf_text_n(&surf, x, y, longstr, INT_MAX, 0xFFFFFF);
		surf_text_n(&surf, x, y, longstr, -5, 0xFFFFFF);
		calls += 5;
		if (!guards_ok()) fail("text wrote outside the surface", x, y, 0, 0);
	}
	if (surf_text(&surf, 0, 0, "abc", 0) != 3 * STK_FONT_W) fail("text width", 0, 0, 0, 0);
	if (surf_text_n(&surf, 0, 0, "abcdef", 2, 0) != 2 * STK_FONT_W) fail("text_n width", 0, 0, 0, 0);
	if (surf_text(&surf, 0, 0, (const char *)0, 0) != 0) fail("NULL string", 0, 0, 0, 0);
	/* Something visible actually lands for an in-bounds glyph. */
	reset();
	surf_text(&surf, 2, 2, "M", 0x00FFFF);
	int ink = 0;
	for (int i = 0; i < W * H; i++)
		if (surf.px[i] == 0x00FFFF)
			ink++;
	if (ink < 10) fail("glyph drew almost nothing", ink, 0, 0, 0);
}

static void test_blit(void)
{
	enum { SW = 9, SH = 7 };
	static unsigned int src[SW * SH];
	for (int i = 0; i < SW * SH; i++)
		src[i] = 0x00100000u + (unsigned int)i;

	for (int xi = 0; xi < NEDGE; xi++)
	for (int yi = 0; yi < NEDGE; yi++) {
		int x = EDGE[xi], y = EDGE[yi];
		reset();
		surf_blit(&surf, x, y, SW, SH, src);
		calls++;
		if (!guards_ok()) { fail("blit wrote outside the surface", x, y, SW, SH); continue; }
		for (int py = 0; py < H; py++)
			for (int px = 0; px < W; px++) {
				long long sx = (long long)px - x, sy = (long long)py - y;
				int in = sx >= 0 && sx < SW && sy >= 0 && sy < SH;
				unsigned int want = in ? src[sy * SW + sx] : BG;
				if (surf.px[py * W + px] != want) {
					fail("blit copied the wrong pixels", x, y, px, py);
					py = H;
					break;
				}
			}
	}
	reset();
	surf_blit(&surf, 0, 0, -3, 5, src);
	surf_blit(&surf, 0, 0, 5, INT_MIN, src);
	surf_blit(&surf, 0, 0, SW, SH, (const unsigned int *)0);
	if (!guards_ok()) fail("degenerate blit wrote outside", 0, 0, 0, 0);
}

static void test_degenerate_surfaces(void)
{
	struct stk_surface none = { (unsigned int *)0, W, H };
	struct stk_surface flat = { surf.px, 0, H };
	struct stk_surface neg  = { surf.px, -4, -4 };
	const struct stk_surface *bad[] = { (const struct stk_surface *)0, &none, &flat, &neg };
	reset();
	for (int i = 0; i < 4; i++) {
		surf_pixel(bad[i], 1, 1, 0xFFFFFF);
		surf_fill(bad[i], 0, 0, 10, 10, 0xFFFFFF);
		surf_rect(bad[i], 0, 0, 10, 10, 0xFFFFFF);
		surf_line(bad[i], 0, 0, 10, 10, 0xFFFFFF);
		surf_text(bad[i], 0, 0, "hello", 0xFFFFFF);
		surf_blit(bad[i], 0, 0, 2, 2, surf.px);
	}
	for (int i = 0; i < W * H; i++)
		if (surf.px[i] != BG) { fail("degenerate surface was drawn to", i, 0, 0, 0); break; }
	if (!guards_ok()) fail("degenerate surface wrote outside", 0, 0, 0, 0);
}

int main(void)
{
	test_fill();
	test_rect_line_pixel();
	test_text();
	test_blit();
	test_degenerate_surfaces();
	printf("%ld adversarial calls, %d failure(s)\n", calls, failures);
	return failures ? 1 : 0;
}
