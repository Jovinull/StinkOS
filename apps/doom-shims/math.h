/* <math.h> shim. Doom is fixed-point throughout (m_fixed.c, tables.c) and
 * the real-time trig calls in r_main.c sit under `#if 0`, so almost nothing
 * from libm is reachable -- except for one fabs() inside v_video.c that
 * needs to compile even though !usemouse short-circuits the call before it
 * runs. fabs is a one-liner; bring in the rest as declarations only and
 * trust --gc-sections to strip what nothing references. */
#ifndef _STINK_MATH_H
#define _STINK_MATH_H

#define M_PI 3.14159265358979323846

static inline double fabs(double x) { return x < 0 ? -x : x; }
static inline float  fabsf(float x) { return x < 0 ? -x : x; }

/* Declarations only, for completeness. None of the trig is reachable in the
 * Doom code paths we build, but a careless future include shouldn't fail
 * with "implicit declaration" if it surfaces. */
double sin(double x);
double cos(double x);
double tan(double x);
double atan(double x);
double atan2(double y, double x);
double sqrt(double x);
double pow(double x, double y);
double floor(double x);
double ceil(double x);

#endif
