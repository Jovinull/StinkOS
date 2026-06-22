/* Linear framebuffer drawing primitives (set up from the VBE mode). */
#ifndef FB_H
#define FB_H

struct vbe_mode;

void fb_init(const struct vbe_mode *m);
void fb_putpixel(unsigned int x, unsigned int y, unsigned int rgb);
void fb_fill(unsigned int rgb);
void fb_rect(unsigned int x0, unsigned int y0,
             unsigned int w, unsigned int h, unsigned int rgb);

#endif
