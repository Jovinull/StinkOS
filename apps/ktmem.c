/* Helper image for the ktest `oom` case: take memory until the kernel
 * refuses, write a pattern through all of it, read it back, release it.
 * Exit 42 when every byte survived, 43 when memory did not hold what was
 * written (the kernel handed out a frame that is not real RAM, or one that
 * someone else also owns), 44 when nothing at all could be mapped. */
#include "libstink.h"

#define CHUNK (256 * 1024)
#define MAXC  56

static unsigned char *chunk[MAXC];

void main(void)
{
	int n = 0;
	while (n < MAXC) {
		unsigned char *p = sys_mmap(CHUNK);
		if (!p)
			break;
		for (unsigned int off = 0; off < CHUNK; off += 512)
			p[off] = (unsigned char)(n * 17 + (off >> 9) + 5);
		chunk[n++] = p;
	}
	if (n == 0)
		sys_exit_code(44);
	for (int i = 0; i < n; i++)
		for (unsigned int off = 0; off < CHUNK; off += 512)
			if (chunk[i][off] != (unsigned char)(i * 17 + (off >> 9) + 5))
				sys_exit_code(43);
	for (int i = 0; i < n; i++)
		sys_munmap(chunk[i], CHUNK);
	sys_exit_code(42);
}
