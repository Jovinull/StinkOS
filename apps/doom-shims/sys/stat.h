/* <sys/stat.h> shim. Minimal stubs so the Doom build links: we don't model a
 * permission/inode system at all, so the calls always succeed and report
 * sensible defaults. */
#ifndef _STINK_SYS_STAT_H
#define _STINK_SYS_STAT_H

#include <sys/types.h>
#include <time.h>

struct stat {
	mode_t  st_mode;
	off_t   st_size;
	time_t  st_mtime;
};

#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)

int stat(const char *path, struct stat *buf);
int mkdir(const char *path, mode_t mode);

#endif
