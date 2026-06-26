/* <assert.h> shim. Aborts via SYS_EXIT (no useful core dump on this OS) and
 * logs the file/line through the serial console so the failure is visible. */
#ifndef _STINK_ASSERT_H
#define _STINK_ASSERT_H
#include "libstink.h"

#ifdef NDEBUG
#define assert(x) ((void)0)
#else
#define assert(x) do {                                                       \
	if (!(x)) {                                                              \
		sys_printf("assert: %s:%d: %s\n", __FILE__, __LINE__, #x);           \
		abort();                                                             \
	}                                                                        \
} while (0)
#endif
#endif
