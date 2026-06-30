/* libstink_font.h — 8×16 userland bitmap font for StinkOS.
 * Renders text strings via sys_blit (one syscall per string).
 * Font covers ASCII 32-126. Characters outside range render as space. */
#pragma once
#include <stddef.h>

/* Draw a NUL-terminated string at pixel (x, y) with given RGB colour.
 * Background is transparent (skips 0-bits; caller fills BG beforehand).
 * Returns pixel width consumed (8 * number of chars drawn). */
int  stink_font_str(int x, int y, const char *s, unsigned int rgb);
int  stink_font_strn(int x, int y, const char *s, size_t n, unsigned int rgb);
void stink_font_char(int x, int y, char c, unsigned int rgb);

/* Font metrics */
#define STINK_FONT_W  8
#define STINK_FONT_H  16
