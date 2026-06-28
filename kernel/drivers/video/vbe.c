/* Reads the VBE ModeInfoBlock the bootloader stored in low memory.
 * Field offsets follow the VBE 2.0 ModeInfoBlock layout. */
#include "vbe.h"
#include "memlayout.h"

/* Phys address of the VBE block written by boot.s. Must match MODE_INFO
 * in boot.s. We read through P2V() because the kernel pgdir no longer
 * identity-maps low phys (xv6 higher-half rule); the high-half mirror
 * sits at P2V(0x0700) = 0x80000700. */
#define MODE_INFO_PHYS 0x0700u
#define MODE_INFO_VIRT P2V(MODE_INFO_PHYS)

#define OFF_PITCH 16
#define OFF_XRES  18
#define OFF_YRES  20
#define OFF_BPP   25
#define OFF_FB    40

void vbe_read(struct vbe_mode *m)
{
	unsigned short pitch = *(volatile unsigned short *)(MODE_INFO_VIRT + OFF_PITCH);
	unsigned short xres  = *(volatile unsigned short *)(MODE_INFO_VIRT + OFF_XRES);
	unsigned short yres  = *(volatile unsigned short *)(MODE_INFO_VIRT + OFF_YRES);
	unsigned char  bpp   = *(volatile unsigned char  *)(MODE_INFO_VIRT + OFF_BPP);
	unsigned int   fb    = *(volatile unsigned int    *)(MODE_INFO_VIRT + OFF_FB);

	m->width = xres;
	m->height = yres;
	m->pitch = pitch;
	m->bpp = bpp;
	m->framebuffer = fb;
	m->valid = (xres != 0 && yres != 0 && fb != 0);
}
