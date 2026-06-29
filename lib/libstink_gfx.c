/* libstink_gfx.c -- Graphics primitives for StinkOS.
 *
 * Provides vector drawing capabilities on top of sys_draw/sys_fillrect.
 */

#include "libstink.h"

/* Absolute value helper for ints */
static inline int int_abs(int x) {
    return x < 0 ? -x : x;
}

/* sys_drawline -- Bresenham's line algorithm
 *
 * Draws a straight line between (x0, y0) and (x1, y1) with the specified RGB color.
 * Uses the sys_draw syscall for each pixel. For performance, horizontal or vertical
 * lines are delegated to sys_fillrect.
 */
void sys_drawline(int x0, int y0, int x1, int y1, unsigned int rgb)
{
    /* Optimization for straight horizontal or vertical lines */
    if (x0 == x1) {
        int y = y0 < y1 ? y0 : y1;
        int h = int_abs(y1 - y0) + 1;
        sys_fillrect(x0, y, 1, h, rgb);
        return;
    }
    if (y0 == y1) {
        int x = x0 < x1 ? x0 : x1;
        int w = int_abs(x1 - x0) + 1;
        sys_fillrect(x, y0, w, 1, rgb);
        return;
    }

    /* Standard Bresenham */
    int dx = int_abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -int_abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int e2;

    for (;;) {
        sys_draw(x0, y0, rgb);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}
