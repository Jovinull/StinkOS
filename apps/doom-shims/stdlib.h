/* <stdlib.h> shim. malloc/free/calloc/realloc, exit/abort, atoi/strtol, rand
 * and abs all live in libstink.h. */
#ifndef _STINK_STDLIB_H
#define _STINK_STDLIB_H
#include "libstink.h"

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

/* getenv is queried by Doom for things like DOOMWADDIR. We have no environment,
 * so always report unset; Doom falls back to the working-directory search path
 * which is what we want anyway. */
static inline char *getenv(const char *name) { (void)name; return (char *)0; }

double atof(const char *s);
#endif
