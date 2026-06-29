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
