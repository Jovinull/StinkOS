/* libstink_surface.c -- clipped drawing into a pixel buffer.
 * See libstink_surface.h for the contract. Nothing here makes a syscall,
 * so the file also builds for the host (tests/test_surface.c). */
#include "libstink_surface.h"
#include "libstink_font.h"

/* Clip the span [pos, pos+len) to [0, limit). Returns the clipped length
 * (0 when nothing is left) and stores the clipped start. Done in 64 bits
 * so pos + len cannot wrap. */
static int clip_span(int pos, int len, int limit, int *start)
{
	if (len <= 0 || limit <= 0)
		return 0;
	long long a = pos, b = (long long)pos + len;
	if (a < 0)
		a = 0;
	if (b > limit)
		b = limit;
	if (b <= a)
		return 0;
	*start = (int)a;
	return (int)(b - a);
}

static int usable(const struct stk_surface *s)
{
	return s && s->px && s->w > 0 && s->h > 0;
}

void surf_pixel(const struct stk_surface *s, int x, int y, unsigned int rgb)
{
	if (!usable(s) || x < 0 || y < 0 || x >= s->w || y >= s->h)
		return;
	s->px[(unsigned int)y * (unsigned int)s->w + (unsigned int)x] = rgb & 0x00FFFFFFu;
}

void surf_fill(const struct stk_surface *s, int x, int y, int w, int h, unsigned int rgb)
{
	if (!usable(s))
		return;
	int x0, y0;
	int cw = clip_span(x, w, s->w, &x0);
	int ch = clip_span(y, h, s->h, &y0);
	if (!cw || !ch)
		return;
	rgb &= 0x00FFFFFFu;
	for (int row = 0; row < ch; row++) {
		unsigned int *p = s->px + (unsigned int)(y0 + row) * (unsigned int)s->w + x0;
		for (int col = 0; col < cw; col++)
			p[col] = rgb;
	}
}

void surf_rect(const struct stk_surface *s, int x, int y, int w, int h, unsigned int rgb)
{
	if (w <= 0 || h <= 0)
		return;
	surf_fill(s, x, y, w, 1, rgb);
	surf_fill(s, x, (int)((long long)y + h - 1), w, 1, rgb);
	surf_fill(s, x, y, 1, h, rgb);
	surf_fill(s, (int)((long long)x + w - 1), y, 1, h, rgb);
}

/* Cohen-Sutherland outcode of a point against the surface. */
static int outcode(long long x, long long y, int w, int h)
{
	int c = 0;
	if (x < 0)       c |= 1;
	else if (x >= w) c |= 2;
	if (y < 0)       c |= 4;
	else if (y >= h) c |= 8;
	return c;
}

void surf_line(const struct stk_surface *s, int x0, int y0, int x1, int y1, unsigned int rgb)
{
	if (!usable(s))
		return;
	/* Clip the segment to the surface first: stepping a line whose ends
	 * are billions of pixels away would never finish. */
	/* Keep the intersection arithmetic below far from 64-bit overflow.
	 * Only absurd endpoints are affected; their direction shifts a
	 * little, their in-bounds guarantee does not. */
	const int lim = 1 << 24;
	if (x0 > lim) x0 = lim;
	if (x0 < -lim) x0 = -lim;
	if (y0 > lim) y0 = lim;
	if (y0 < -lim) y0 = -lim;
	if (x1 > lim) x1 = lim;
	if (x1 < -lim) x1 = -lim;
	if (y1 > lim) y1 = lim;
	if (y1 < -lim) y1 = -lim;
	long long ax = x0, ay = y0, bx = x1, by = y1;
	int ca = outcode(ax, ay, s->w, s->h), cb = outcode(bx, by, s->w, s->h);
	for (int guard = 0; guard < 8; guard++) {
		if (!(ca | cb))
			break;                             /* both ends inside */
		if (ca & cb)
			return;                            /* entirely on one outside */
		int c = ca ? ca : cb;
		long long x, y;
		if (c & 8) {
			x = ax + (bx - ax) * ((s->h - 1) - ay) / (by - ay);
			y = s->h - 1;
		} else if (c & 4) {
			x = ax + (bx - ax) * (0 - ay) / (by - ay);
			y = 0;
		} else if (c & 2) {
			y = ay + (by - ay) * ((s->w - 1) - ax) / (bx - ax);
			x = s->w - 1;
		} else {
			y = ay + (by - ay) * (0 - ax) / (bx - ax);
			x = 0;
		}
		if (c == ca) {
			ax = x; ay = y;
			ca = outcode(ax, ay, s->w, s->h);
		} else {
			bx = x; by = y;
			cb = outcode(bx, by, s->w, s->h);
		}
	}
	if (ca | cb)
		return;

	int px = (int)ax, py = (int)ay, ex = (int)bx, ey = (int)by;
	int dx = ex > px ? ex - px : px - ex, dy = ey > py ? ey - py : py - ey;
	int sx = px < ex ? 1 : -1, sy = py < ey ? 1 : -1;
	int err = dx - dy;
	for (;;) {
		surf_pixel(s, px, py, rgb);
		if (px == ex && py == ey)
			break;
		int e2 = err * 2;
		if (e2 > -dy) { err -= dy; px += sx; }
		if (e2 <  dx) { err += dx; py += sy; }
	}
}

static void glyph(const struct stk_surface *s, int x, int y, char c, unsigned int rgb)
{
	/* Whole glyph outside? Skip without touching rows. */
	if (x <= -STK_FONT_W || y <= -STK_FONT_H || x >= s->w || y >= s->h)
		return;
	const unsigned char *g = stink_font_get_glyph(c);
	for (int row = 0; row < STK_FONT_H; row++) {
		unsigned char bits = g[row];
		if (!bits)
			continue;
		for (int col = 0; col < STK_FONT_W; col++)
			if (bits & (0x80u >> col))
				surf_pixel(s, x + col, y + row, rgb);
	}
}

int surf_text_n(const struct stk_surface *s, int x, int y, const char *str, int n,
                unsigned int rgb)
{
	if (!str || n <= 0)
		return 0;
	int drawn = 0;
	long long cx = x;
	for (; drawn < n && str[drawn]; drawn++, cx += STK_FONT_W) {
		if (usable(s) && cx > -STK_FONT_W && cx < s->w)
			glyph(s, (int)cx, y, str[drawn], rgb);
	}
	return drawn * STK_FONT_W;
}

int surf_text(const struct stk_surface *s, int x, int y, const char *str, unsigned int rgb)
{
	return surf_text_n(s, x, y, str, 0x7FFFFFFF / STK_FONT_W, rgb);
}

void surf_blit(const struct stk_surface *s, int x, int y, int w, int h,
               const unsigned int *src)
{
	if (!usable(s) || !src || w <= 0 || h <= 0)
		return;
	int x0, y0;
	int cw = clip_span(x, w, s->w, &x0);
	int ch = clip_span(y, h, s->h, &y0);
	if (!cw || !ch)
		return;
	/* (x0 - x, y0 - y) is where the visible part starts inside src. */
	long long sx = (long long)x0 - x, sy = (long long)y0 - y;
	for (int row = 0; row < ch; row++) {
		const unsigned int *sp = src + (sy + row) * w + sx;
		unsigned int *dp = s->px + (unsigned int)(y0 + row) * (unsigned int)s->w + x0;
		for (int col = 0; col < cw; col++)
			dp[col] = sp[col] & 0x00FFFFFFu;
	}
}
