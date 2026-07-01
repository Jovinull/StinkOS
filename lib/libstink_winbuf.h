/* libstink_winbuf.h — userspace pixel buffer drawing for windowed apps. */
#pragma once

/* Fill rectangle in pixel buffer. buf = USER_WIN_BASE, stride = win_w. */
void win_fillrect_buf(unsigned int *buf, unsigned int stride,
                      int x, int y, int w, int h, unsigned int rgb);

/* Draw line in pixel buffer. sw/sh = buffer dimensions for clipping. */
void win_drawline_buf(unsigned int *buf, unsigned int stride,
                      unsigned int sw, unsigned int sh,
                      int x0, int y0, int x1, int y1, unsigned int rgb);

/* Render one 8×16 glyph at (x,y) into buf. Transparent pixels skipped. */
void win_font_char_buf(unsigned int *buf, unsigned int stride,
                       int x, int y, char c, unsigned int rgb);

/* Render NUL-terminated string. Returns pixel width consumed. */
int win_font_str_buf(unsigned int *buf, unsigned int stride,
                     int x, int y, const char *s, unsigned int rgb);
