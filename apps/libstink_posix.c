/* POSIX-ish glue: a small set of <unistd.h>, <time.h>, <errno.h>, <fcntl.h>
 * and <sys/stat.h> entry points implemented on top of the StinkOS syscall
 * surface. Just enough for ported codebases (Doom in particular) to compile
 * and link without dragging in a real libc. None of these go beyond what
 * libstink.h already exposes -- they only translate names and conventions. */
#include "libstink.h"

#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

/* errno is a real variable, even though the syscall layer never sets it.
 * Doom touches it on a few error paths just to print something useful;
 * giving back a constant 0 is harmless and avoids a build error. */
int errno = 0;

int gettimeofday(struct timeval *tv, struct timezone *tz)
{
	(void)tz;
	if (!tv)
		return -1;

	unsigned int t = sys_ticks();          /* 10 ms ticks since boot */
	tv->tv_sec  = (time_t)(t / 100);
	tv->tv_usec = (long)((t % 100) * 10000);
	return 0;
}

time_t time(time_t *out)
{
	time_t t = (time_t)(sys_ticks() / 100);
	if (out)
		*out = t;
	return t;
}

int usleep(unsigned int us)
{
	/* sys_sleep_ms granularity is 10 ms; round up so sub-tick sleeps still
	 * give the CPU back briefly instead of busy-spinning. */
	unsigned int ms = (us + 999) / 1000;
	if (ms == 0)
		ms = 1;
	sys_sleep_ms(ms);
	return 0;
}

unsigned int sleep(unsigned int s)
{
	sys_sleep_ms(s * 1000);
	return 0;                              /* always slept the full duration */
}

int access(const char *path, int mode)
{
	(void)mode;
	/* No permission model on StinkOS, so "can it be opened" is the only
	 * meaningful check. fs_file_read with size 0 returns 0 when the file
	 * exists, -1 when it does not. */
	return sys_fread(path, (void *)0, 0) >= 0 ? 0 : -1;
}

int unlink(const char *path)
{
	return sys_fdelete(path);
}

int open(const char *path, int flags, ...)
{
	/* StinkOS only honours O_CREAT (mapped to SYS_O_CREATE); everything
	 * else collapses into the underlying create-or-open behaviour. The
	 * variadic mode arg is parsed and ignored by the caller convention. */
	int kflags = (flags & O_CREAT) ? SYS_O_CREATE : 0;
	return sys_open(path, kflags);
}

int stat(const char *path, struct stat *buf)
{
	if (!buf)
		return -1;

	int fd = sys_open(path, 0);
	if (fd < 0)
		return -1;
	int size = sys_seek(fd, 0, SYS_SEEK_END);
	sys_close(fd);
	if (size < 0)
		return -1;

	buf->st_mode  = S_IFREG | 0644;        /* regular file, rw-r--r-- */
	buf->st_size  = (off_t)size;
	buf->st_mtime = 0;
	return 0;
}

int mkdir(const char *path, mode_t mode)
{
	(void)path;
	(void)mode;
	return 0;                              /* no directories in StinkFS */
}
