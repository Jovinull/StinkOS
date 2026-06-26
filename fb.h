/* Linear framebuffer drawing primitives (set up from the VBE mode). */
#ifndef FB_H
#define FB_H

struct vbe_mode;

void fb_init(const struct vbe_mode *m);
unsigned int fb_phys_base(void);    /* physical address of the VBE linear framebuffer */
unsigned int fb_stride_px(void);    /* screen width in pixels (== pitch / bytes_per_pixel) */
void fb_putpixel(unsigned int x, unsigned int y, unsigned int rgb);
unsigned int fb_getpixel(unsigned int x, unsigned int y);
void fb_fill(unsigned int rgb);
void fb_rect(unsigned int x0, unsigned int y0,
             unsigned int w, unsigned int h, unsigned int rgb);
void fb_blit(unsigned int x0, unsigned int y0, unsigned int w, unsigned int h,
             const unsigned int *src);
void fb_char(unsigned int x, unsigned int y, char c, unsigned int rgb);
void fb_text(unsigned int x, unsigned int y, const char *s, unsigned int rgb);

#endif
