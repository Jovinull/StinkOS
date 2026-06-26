/* <unistd.h> shim. Doom touches a handful of POSIX file primitives -- mostly
 * to test for existence and to delete temp save-game files. The kernel VFS
 * fills in the real work; here we just provide the prototypes. */
#ifndef _STINK_UNISTD_H
#define _STINK_UNISTD_H

#include "libstink.h"

#define F_OK 0   /* file exists                          */
#define R_OK 4   /* readable -- always true if it exists */
#define W_OK 2   /* writable -- ditto                    */
#define X_OK 1

int access(const char *path, int mode);
int unlink(const char *path);
int usleep(unsigned int us);
unsigned int sleep(unsigned int s);

#endif
