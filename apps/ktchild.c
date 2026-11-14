/* Helper image for ktest: something for a forked child to exec. Grows its
 * own heap (which is what used to clobber the parent's break when the
 * kernel kept one global heap watermark), touches it, and exits with a
 * recognisable code. */
#include "libstink.h"

void main(void)
{
	unsigned char *p = sys_sbrk(16 * 1024);
	if (p == (void *)-1)
		sys_exit_code(40);
	for (int i = 0; i < 16 * 1024; i++)
		p[i] = (unsigned char)i;
	sys_exit_code(42);
}
