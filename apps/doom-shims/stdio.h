/* <stdio.h> shim for ported C code (Doom). All real declarations live in
 * libstink.h; this header just makes `#include <stdio.h>` resolve. Doom and
 * many similar codebases assume the standard library is present, so the
 * shim layer lets them compile unchanged. */
#ifndef _STINK_STDIO_H
#define _STINK_STDIO_H
#include "libstink.h"
#endif
