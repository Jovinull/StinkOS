/* <strings.h> shim (the POSIX file, NOT <string.h>). doomtype.h pulls this
 * in unconditionally for strcasecmp / strncasecmp -- both live in
 * libstink_posix.c with the same prototype. */
#ifndef _STINK_STRINGS_H
#define _STINK_STRINGS_H

#include <stddef.h>

int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, unsigned int n);

#endif
