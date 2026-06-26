/* <string.h> shim. Real string and memory primitives live in libstink.h. */
#ifndef _STINK_STRING_H
#define _STINK_STRING_H
#include "libstink.h"

/* strerror is rarely called by Doom but appears in formatted error paths.
 * We have no errno table, so just return a constant placeholder rather than
 * pulling in a string table. */
static inline char *strerror(int errnum) { (void)errnum; return "stinkos: error"; }
#endif
