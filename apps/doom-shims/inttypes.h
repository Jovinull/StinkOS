/* <inttypes.h> shim. The integer types come in from gcc's freestanding
 * <stdint.h>; we only need to provide the printf/scanf format macros that
 * doom code uses (mostly PRIx32 / PRIu32 for hex dumps). */
#ifndef _STINK_INTTYPES_H
#define _STINK_INTTYPES_H

#include <stdint.h>

#define PRId8   "d"
#define PRIu8   "u"
#define PRIx8   "x"
#define PRIX8   "X"
#define PRId16  "d"
#define PRIu16  "u"
#define PRIx16  "x"
#define PRIX16  "X"
#define PRId32  "d"
#define PRIu32  "u"
#define PRIx32  "x"
#define PRIX32  "X"
#define PRId64  "lld"
#define PRIu64  "llu"
#define PRIx64  "llx"
#define PRIX64  "llX"

#endif
