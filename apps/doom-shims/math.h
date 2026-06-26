/* <math.h> shim. Doom is fixed-point everywhere (m_fixed.c, tables.c), so the
 * standard math functions are essentially never called -- but a couple of
 * Doom sources include <math.h> unconditionally, and this empty stub keeps
 * the build moving. M_PI is the one constant that does appear. */
#ifndef _STINK_MATH_H
#define _STINK_MATH_H

#define M_PI 3.14159265358979323846

#endif
