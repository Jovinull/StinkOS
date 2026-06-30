/* libstink_gfx.c -- Graphics primitives for StinkOS.
 *
 * Provides vector drawing capabilities on top of sys_draw/sys_fillrect.
 */

#include "libstink.h"

#define SCREEN_W 1024
#define SCREEN_H  768

static inline int int_abs(int x) {
    return x < 0 ? -x : x;
}

/* sys_drawline -- Bresenham's line algorithm.
 *
 * Coordinates may exceed screen bounds; out-of-bounds pixels are skipped so
 * callers never need to pre-clip and corrupt memory can never happen.
 */
void sys_drawline(int x0, int y0, int x1, int y1, unsigned int rgb)
{
    if (x0 == x1) {
        if (x0 < 0 || x0 >= SCREEN_W) return;
        int y = y0 < y1 ? y0 : y1;
        int h = int_abs(y1 - y0) + 1;
        if (y < 0)          { h += y; y = 0; }
        if (y + h > SCREEN_H) h = SCREEN_H - y;
        if (h > 0) sys_fillrect(x0, y, 1, h, rgb);
        return;
    }
    if (y0 == y1) {
        if (y0 < 0 || y0 >= SCREEN_H) return;
        int x = x0 < x1 ? x0 : x1;
        int w = int_abs(x1 - x0) + 1;
        if (x < 0)          { w += x; x = 0; }
        if (x + w > SCREEN_W) w = SCREEN_W - x;
        if (w > 0) sys_fillrect(x, y0, w, 1, rgb);
        return;
    }

    int dx  =  int_abs(x1 - x0);
    int sx  = x0 < x1 ? 1 : -1;
    int dy  = -int_abs(y1 - y0);
    int sy  = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int e2;

    for (;;) {
        if ((unsigned)x0 < (unsigned)SCREEN_W && (unsigned)y0 < (unsigned)SCREEN_H)
            sys_draw(x0, y0, rgb);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* ---- Rounded rectangle, shadow, window frame ---- */

/* Integer square root via Newton's method. */
static int gfx_isqrt(int n)
{
    if (n <= 0) return 0;
    int x = n, y = (x + 1) >> 1;
    while (y < x) { x = y; y = (x + n / x) >> 1; }
    return x;
}

/* Rectangle outline — 1 px border, no fill. */
void draw_rect_outline(int x, int y, int w, int h, unsigned int rgb)
{
    sys_fillrect(x,         y,         w, 1, rgb); /* top */
    sys_fillrect(x,         y + h - 1, w, 1, rgb); /* bottom */
    sys_fillrect(x,         y,         1, h, rgb); /* left */
    sys_fillrect(x + w - 1, y,         1, h, rgb); /* right */
}

/* Filled rounded rectangle.
 *
 * Uses two overlapping fillrects for the interior cross, then fills each
 * corner quadrant with horizontal spans derived from a circle of radius r.
 * Based on the same per-quadrant scan approach used by ToaruOS graphics.c
 * (draw_rounded_rectangle_pattern), adapted to use StinkOS fillrect calls.
 */
void draw_rounded_rect(int x, int y, int w, int h, int r, unsigned int rgb)
{
    if (w <= 0 || h <= 0) return;
    if (r <= 0) { sys_fillrect(x, y, w, h, rgb); return; }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    /* Interior: vertical strip (full height) + horizontal strip (full width) */
    sys_fillrect(x + r, y,     w - 2 * r, h,         rgb);
    sys_fillrect(x,     y + r, w,         h - 2 * r, rgb);

    /* Corner quarter-circles as horizontal spans */
    for (int i = 0; i < r; i++) {
        int span = gfx_isqrt(r * r - i * i);
        if (span <= 0) continue;
        /* Top row for this step (counting up from the corner centre) */
        sys_fillrect(x + r - span,  y + r - 1 - i, span, 1, rgb);  /* top-left  */
        sys_fillrect(x + w - r,     y + r - 1 - i, span, 1, rgb);  /* top-right */
        /* Bottom row */
        sys_fillrect(x + r - span,  y + h - r + i, span, 1, rgb);  /* bot-left  */
        sys_fillrect(x + w - r,     y + h - r + i, span, 1, rgb);  /* bot-right */
    }
}

/* Simple drop-shadow: draws a dark rectangle offset by (depth, depth) behind
 * the caller's widget.  Call before drawing the widget itself. */
void draw_shadow(int x, int y, int w, int h, int depth, unsigned int shadow_rgb)
{
    if (depth <= 0 || w <= 0 || h <= 0) return;
    sys_fillrect(x + depth, y + depth, w, h, shadow_rgb);
}

/* Window decoration: titlebar, close button, outer border.
 *
 * Layout derived from ToaruOS decor-fancy (TITLEBAR_HEIGHT=33, close button
 * at right, title left-aligned), colours adapted to the StinkOS dark palette.
 *
 * Returns the Y coordinate of the inner content area (titlebar bottom + 1).
 * The caller is responsible for drawing content below that Y.
 */
#define WIN_TITLEBAR_H  33
#define WIN_CLOSE_W     24
#define WIN_CLOSE_H     20

int draw_window_frame(int x, int y, int w, int h, const char *title)
{
    /* Shadow behind the window */
    draw_shadow(x, y, w, h, 4, 0x050810);

    /* Window background */
    sys_fillrect(x, y, w, h, 0x0d1117);

    /* Titlebar strip */
    sys_fillrect(x, y, w, WIN_TITLEBAR_H, 0x161b22);

    /* Title text: vertically centred in titlebar */
    sys_drawtext(x + 12, y + (WIN_TITLEBAR_H - 8) / 2, title, 0xe6edf3);

    /* Close button: rounded red rect with X, ToaruOS-style, right side */
    int bx = x + w - WIN_CLOSE_W - 8;
    int by = y + (WIN_TITLEBAR_H - WIN_CLOSE_H) / 2;
    draw_rounded_rect(bx, by, WIN_CLOSE_W, WIN_CLOSE_H, 4, 0xf47067);
    sys_drawtext(bx + (WIN_CLOSE_W - 8) / 2, by + (WIN_CLOSE_H - 8) / 2, "X", 0xffffff);

    /* Separator line below titlebar */
    sys_fillrect(x, y + WIN_TITLEBAR_H, w, 1, 0x30363d);

    /* Outer border */
    sys_fillrect(x,             y,             w, 1, 0x30363d);
    sys_fillrect(x,             y + h - 1,     w, 1, 0x30363d);
    sys_fillrect(x,             y,             1, h, 0x30363d);
    sys_fillrect(x + w - 1,     y,             1, h, 0x30363d);

    return y + WIN_TITLEBAR_H + 1;
}
