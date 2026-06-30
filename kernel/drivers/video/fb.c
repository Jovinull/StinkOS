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
	/* Clip BEFORE entering the per-pixel loops. fb_putpixel rejects
	 * out-of-bounds writes on its own, but a userland call with
	 * w = h = 0xFFFFFFFF would otherwise spin the kernel through ~10^19
	 * rejected pixel calls instead of returning immediately. */
	if (x0 >= width || y0 >= height)
		return;
	if (w > width  - x0) w = width  - x0;
	if (h > height - y0) h = height - y0;
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

/* Blit n pixels from src at screen position (x, y). Used by the compositor
 * to copy page-aligned segments of a window buffer row by row. */
void fb_blit_row(unsigned int x, unsigned int y, unsigned int n,
                 const unsigned int *src)
{
	if (!src || x >= width || y >= height) return;
	if (x + n > width) n = width - x;
	volatile unsigned char *p = fb + y * pitch + x * bytes_pp;
	for (unsigned int i = 0; i < n; i++) {
		unsigned int rgb = src[i];
		p[0] =  rgb        & 0xFF;
		p[1] = (rgb >> 8)  & 0xFF;
		p[2] = (rgb >> 16) & 0xFF;
		if (bytes_pp == 4) p[3] = 0;
		p += bytes_pp;
	}
}

/* Nearest-neighbour upscale / downscale of an ARGB source onto the
 * framebuffer. Computes the source pixel via integer ratio per dst pixel.
 * No interpolation -- the goal is "fast enough to scale Doom's 640x400 to
 * 1024x640 every frame on a 386", not "pretty". Clipped to the visible
 * area. Bails on degenerate dimensions instead of dividing by zero. */
void fb_blit_scaled(unsigned int x0, unsigned int y0,
                    unsigned int dst_w, unsigned int dst_h,
                    const unsigned int *src,
                    unsigned int src_w, unsigned int src_h)
{
	if (!src || dst_w == 0 || dst_h == 0 || src_w == 0 || src_h == 0)
		return;
	if (x0 >= width || y0 >= height)
		return;

	unsigned int cw = dst_w;
	unsigned int ch = dst_h;
	if (x0 + cw > width)  cw = width  - x0;
	if (y0 + ch > height) ch = height - y0;

	for (unsigned int row = 0; row < ch; row++) {
		unsigned int sy = (row * src_h) / dst_h;
		const unsigned int *srow = src + sy * src_w;
		volatile unsigned char *p = fb + (y0 + row) * pitch + x0 * bytes_pp;
		for (unsigned int col = 0; col < cw; col++) {
			unsigned int sx = (col * src_w) / dst_w;
			unsigned int rgb = srow[sx];
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
