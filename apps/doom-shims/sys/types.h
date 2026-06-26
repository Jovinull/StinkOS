/* <sys/types.h> shim. size_t comes in from gcc's freestanding stddef.h; the
 * rest of these are POSIX integer aliases the rest of the Doom build expects. */
#ifndef _STINK_SYS_TYPES_H
#define _STINK_SYS_TYPES_H

#include <stddef.h>

typedef int           ssize_t;
typedef int           pid_t;
typedef long          off_t;
typedef unsigned int  mode_t;
typedef int           uid_t;
typedef int           gid_t;

#endif
