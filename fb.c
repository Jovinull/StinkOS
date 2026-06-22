/* Linear framebuffer drawing. Colours are passed as 0xRRGGBB and written in the
 * common little-endian B,G,R[,X] byte order used by VBE true-colour modes. */
#include "fb.h"
#include "vbe.h"
#include "font.h"

static volatile unsigned char *fb;
static unsigned int pitch;
static unsigned int width;
static unsigned int height;
static unsigned int bytes_pp;

void fb_init(const struct vbe_mode *m)
{
	fb = (volatile unsigned char *)m->framebuffer;
	pitch = m->pitch;
	width = m->width;
	height = m->height;
	bytes_pp = m->bpp / 8;
}

void fb_putpixel(unsigned int x, unsigned int y, unsigned int rgb)
{
	if (x >= width || y >= height)
		return;

	volatile unsigned char *p = fb + y * pitch + x * bytes_pp;
	p[0] = rgb & 0xFF;             /* blue  */
	p[1] = (rgb >> 8) & 0xFF;      /* green */
	p[2] = (rgb >> 16) & 0xFF;     /* red   */
	if (bytes_pp == 4)
		p[3] = 0;
}

void fb_fill(unsigned int rgb)
{
	for (unsigned int y = 0; y < height; y++)
		for (unsigned int x = 0; x < width; x++)
			fb_putpixel(x, y, rgb);
}

void fb_rect(unsigned int x0, unsigned int y0,
             unsigned int w, unsigned int h, unsigned int rgb)
{
	for (unsigned int y = 0; y < h; y++)
		for (unsigned int x = 0; x < w; x++)
			fb_putpixel(x0 + x, y0 + y, rgb);
}

void fb_char(unsigned int x, unsigned int y, char c, unsigned int rgb)
{
	const unsigned char *glyph = font8x8[(unsigned char)c & 0x7F];

	for (int row = 0; row < 8; row++)
		for (int col = 0; col < 8; col++)
			if (glyph[row] & (0x80 >> col))
				fb_putpixel(x + col, y + row, rgb);
}

void fb_text(unsigned int x, unsigned int y, const char *s, unsigned int rgb)
{
	for (; *s != '\0'; s++) {
		fb_char(x, y, *s, rgb);
		x += 8;
	}
}
