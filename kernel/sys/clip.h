/* Kernel clipboard: one shared static buffer for copy-paste between apps. */
#ifndef KERNEL_SYS_CLIP_H
#define KERNEL_SYS_CLIP_H

#define CLIP_MAX 4096   /* maximum bytes storable in the clipboard */

/* Replace clipboard with the first 'len' bytes of 'buf'. Truncates at CLIP_MAX.
 * Returns bytes actually stored, or -1 on bad pointer/size. */
int clip_write(const char *buf, unsigned int len);

/* Copy up to 'max' bytes of clipboard content into 'buf'.
 * Returns bytes copied (0 if clipboard is empty). */
int clip_read(char *buf, unsigned int max);

#endif
