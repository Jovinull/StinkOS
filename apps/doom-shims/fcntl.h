/* <fcntl.h> shim. The kernel's open() flags are sparse (just SYS_O_CREATE),
 * so the rest of the POSIX flags are aliased to 0 and silently ignored. */
#ifndef _STINK_FCNTL_H
#define _STINK_FCNTL_H

#include "libstink.h"

#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   SYS_O_CREATE
#define O_TRUNC   0
#define O_APPEND  0
#define O_EXCL    0
#define O_BINARY  0

int open(const char *path, int flags, ...);

#endif
