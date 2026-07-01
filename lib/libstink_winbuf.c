/* libstink_winbuf.c — userspace drawing into a pixel buffer.
 *
 * Used by windowed apps: write ARGB32 pixels into the buffer at
 * USER_WIN_BASE (0x12000000), then call sys_win_flush() to composite.
 * No framebuffer syscalls needed — all writes are direct memory stores.
 *
 * stride = window width in pixels (== win_w passed to sys_win_create).
 */
#include "libstink_font.h"
#include "libstink_winbuf.h"
#include <stddef.h>

/* ── Rectangle fill ─────────────────────────────────────────────────────── */

void win_fillrect_buf(unsigned int *buf, unsigned int stride,
                      int x, int y, int w, int h, unsigned int rgb)
{
    unsigned int argb = 0xFF000000u | rgb;
    for (int row = 0; row < h; row++) {
        unsigned int *p = buf + (unsigned int)(y + row) * stride + (unsigned int)x;
        for (int col = 0; col < w; col++)
            p[col] = argb;
    }
}

/* ── Line draw ──────────────────────────────────────────────────────────── */

static void set_px(unsigned int *buf, unsigned int stride,
                   unsigned int sw, unsigned int sh,
                   int x, int y, unsigned int argb)
{
    if ((unsigned int)x >= sw || (unsigned int)y >= sh) return;
    buf[(unsigned int)y * stride + (unsigned int)x] = argb;
}

void win_drawline_buf(unsigned int *buf, unsigned int stride,
                      unsigned int sw, unsigned int sh,
                      int x0, int y0, int x1, int y1, unsigned int rgb)
{
    unsigned int argb = 0xFF000000u | rgb;
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    int err = adx - ady;
    while (1) {
        set_px(buf, stride, sw, sh, x0, y0, argb);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -ady) { err -= ady; x0 += sx; }
        if (e2 <  adx) { err += adx; y0 += sy; }
    }
}

/* ── Glyph / string rendering ──────────────────────────────────────────── */

/* Render one 8×16 glyph at (x,y) into buf. Transparent pixels skipped. */
void win_font_char_buf(unsigned int *buf, unsigned int stride,
                       int x, int y, char c, unsigned int rgb)
{
    const unsigned char *g = stink_font_get_glyph(c);
    unsigned int argb = 0xFF000000u | rgb;
    for (int row = 0; row < STINK_FONT_H; row++) {
        unsigned char bits = g[row];
        unsigned int *p = buf + (unsigned int)(y + row) * stride + (unsigned int)x;
        for (int col = 0; col < STINK_FONT_W; col++) {
            if (bits & (0x80u >> col))
                p[col] = argb;
        }
    }
}

/* Render NUL-terminated string. Returns pixel width used. */
int win_font_str_buf(unsigned int *buf, unsigned int stride,
                     int x, int y, const char *s, unsigned int rgb)
{
    if (!s) return 0;
    int ox = x;
    while (*s) {
        win_font_char_buf(buf, stride, x, y, *s++, rgb);
        x += STINK_FONT_W;
    }
    return x - ox;
}
