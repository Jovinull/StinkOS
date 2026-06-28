/* Reads the VBE ModeInfoBlock the bootloader stored in low memory.
 * Field offsets follow the VBE 2.0 ModeInfoBlock layout. */
#include "vbe.h"

#define MODE_INFO 0x0700        /* must match MODE_INFO in boot.s */

#define OFF_PITCH 16
#define OFF_XRES  18
#define OFF_YRES  20
#define OFF_BPP   25
#define OFF_FB    40

/* gcc -Wall -Os flags MMIO-style integer-to-pointer reads as "array
 * subscript outside bounds of [...0]" (-Warray-bounds). The pattern is
 * intentional: the bootloader writes the VBE block at fixed low memory,
 * we read it back here. The address truly is "small integer cast to
 * pointer"; we silence the diagnostic only inside this function. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"

void vbe_read(struct vbe_mode *m)
{
	unsigned short pitch = *(volatile unsigned short *)(MODE_INFO + OFF_PITCH);
	unsigned short xres  = *(volatile unsigned short *)(MODE_INFO + OFF_XRES);
	unsigned short yres  = *(volatile unsigned short *)(MODE_INFO + OFF_YRES);
	unsigned char  bpp   = *(volatile unsigned char  *)(MODE_INFO + OFF_BPP);
	unsigned int   fb    = *(volatile unsigned int    *)(MODE_INFO + OFF_FB);

	m->width = xres;
	m->height = yres;
	m->pitch = pitch;
	m->bpp = bpp;
	m->framebuffer = fb;
	m->valid = (xres != 0 && yres != 0 && fb != 0);
}

#pragma GCC diagnostic pop
