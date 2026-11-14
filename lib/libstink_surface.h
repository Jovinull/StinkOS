/* libstink_surface.h -- drawing into a pixel buffer, with clipping.
 *
 * A surface is a rectangle of XRGB pixels in memory: a window's buffer
 * (USER_WIN_BASE after sys_win_create) or any buffer of the caller's.
 * Every primitive clips to the surface. Coordinates and sizes may be
 * negative, far outside it or large enough to overflow when added: the
 * call draws whatever part lands inside and never touches memory outside
 * px[0 .. w*h).
 */
#ifndef LIBSTINK_SURFACE_H
#define LIBSTINK_SURFACE_H

struct stk_surface {
	unsigned int *px;      /* w * h pixels, row-major, 0x00RRGGBB */
	int           w, h;
};

#define STK_FONT_W 8
#define STK_FONT_H 16

void surf_pixel(const struct stk_surface *s, int x, int y, unsigned int rgb);
void surf_fill(const struct stk_surface *s, int x, int y, int w, int h, unsigned int rgb);
/* 1-pixel outline of the rectangle (x, y, w, h). */
void surf_rect(const struct stk_surface *s, int x, int y, int w, int h, unsigned int rgb);
void surf_line(const struct stk_surface *s, int x0, int y0, int x1, int y1, unsigned int rgb);

/* 8x16 text, transparent background. `s` may be NULL. surf_text draws up
 * to the NUL; surf_text_n at most n characters. Both return the width in
 * pixels of what the string occupies (drawn or clipped). */
int  surf_text(const struct stk_surface *s, int x, int y, const char *str, unsigned int rgb);
int  surf_text_n(const struct stk_surface *s, int x, int y, const char *str, int n,
                 unsigned int rgb);

/* Copy a w*h block of pixels from `src` (w pixels per row) to (x, y). */
void surf_blit(const struct stk_surface *s, int x, int y, int w, int h,
               const unsigned int *src);

#endif
