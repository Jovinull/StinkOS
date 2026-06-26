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
static unsigned int fb_phys;      /* raw physical address of the LFB */

void fb_init(const struct vbe_mode *m)
{
	fb       = (volatile unsigned char *)m->framebuffer;
	fb_phys  = m->framebuffer;
	pitch    = m->pitch;
	width    = m->width;
	height   = m->height;
	bytes_pp = m->bpp / 8;
}

unsigned int fb_phys_base(void) { return fb_phys; }
unsigned int fb_stride_px(void) { return width; }   /* pixels per row (== pitch/bytes_pp) */

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

unsigned int fb_getpixel(unsigned int x, unsigned int y)
{
	if (x >= width || y >= height)
		return 0;

	volatile unsigned char *p = fb + y * pitch + x * bytes_pp;
	return ((unsigned int)p[2] << 16) | ((unsigned int)p[1] << 8) | p[0];
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

/* Copies a 'w' by 'h' rectangle of packed 0xXXRRGGBB pixels from 'src' (a
 * row-major user buffer with stride = w) to the framebuffer at (x0, y0). The
 * destination is clipped to the visible area; the source is not -- the caller
 * supplies w * h words. Writes are inlined per row to avoid the per-pixel
 * function-call overhead of fb_putpixel on a 1024x768 surface (~786K pixels). */
void fb_blit(unsigned int x0, unsigned int y0, unsigned int w, unsigned int h,
             const unsigned int *src)
{
	if (x0 >= width || y0 >= height)
		return;

	unsigned int cw = w;
	unsigned int ch = h;
	if (x0 + cw > width)  cw = width  - x0;
	if (y0 + ch > height) ch = height - y0;

	for (unsigned int row = 0; row < ch; row++) {
		volatile unsigned char *p = fb + (y0 + row) * pitch + x0 * bytes_pp;
		const unsigned int     *s = src + row * w;
		for (unsigned int col = 0; col < cw; col++) {
			unsigned int rgb = s[col];
			p[0] =  rgb        & 0xFF;
			p[1] = (rgb >> 8)  & 0xFF;
			p[2] = (rgb >> 16) & 0xFF;
			if (bytes_pp == 4)
				p[3] = 0;
			p += bytes_pp;
		}
	}
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
