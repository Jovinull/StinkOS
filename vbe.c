/* Reads the VBE ModeInfoBlock the bootloader stored in low memory.
 * Field offsets follow the VBE 2.0 ModeInfoBlock layout. */
#include "vbe.h"

#define MODE_INFO 0x0700        /* must match MODE_INFO in boot.s */

#define OFF_XRES  18
#define OFF_YRES  20
#define OFF_BPP   25
#define OFF_FB    40

void vbe_read(struct vbe_mode *m)
{
	unsigned short xres = *(volatile unsigned short *)(MODE_INFO + OFF_XRES);
	unsigned short yres = *(volatile unsigned short *)(MODE_INFO + OFF_YRES);
	unsigned char  bpp  = *(volatile unsigned char  *)(MODE_INFO + OFF_BPP);
	unsigned int   fb   = *(volatile unsigned int    *)(MODE_INFO + OFF_FB);

	m->width = xres;
	m->height = yres;
	m->bpp = bpp;
	m->framebuffer = fb;
	m->valid = (xres != 0 && yres != 0 && fb != 0);
}
