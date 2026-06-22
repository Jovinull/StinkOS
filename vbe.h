/* VBE mode info captured by the bootloader (real mode) for the kernel. */
#ifndef VBE_H
#define VBE_H

struct vbe_mode {
	unsigned short width;
	unsigned short height;
	unsigned char  bpp;
	unsigned int   framebuffer;   /* linear framebuffer physical address */
	int            valid;
};

/* Read the ModeInfoBlock the bootloader left in low memory. */
void vbe_read(struct vbe_mode *m);

#endif
