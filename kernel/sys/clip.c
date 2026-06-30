/* Kernel clipboard — shared text buffer accessible via syscalls 92/93. */
#include "clip.h"

static char   clip_buf[CLIP_MAX];
static unsigned int clip_len;

int clip_write(const char *buf, unsigned int len)
{
	if (!buf || len == 0) {
		clip_len = 0;
		return 0;
	}
	if (len > CLIP_MAX) len = CLIP_MAX;
	for (unsigned int i = 0; i < len; i++)
		clip_buf[i] = buf[i];
	clip_len = len;
	return (int)len;
}

int clip_read(char *buf, unsigned int max)
{
	if (!buf || max == 0 || clip_len == 0) return 0;
	unsigned int n = clip_len < max ? clip_len : max;
	for (unsigned int i = 0; i < n; i++)
		buf[i] = clip_buf[i];
	return (int)n;
}
