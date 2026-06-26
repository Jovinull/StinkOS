/* <time.h> shim. The kernel exposes monotonic ticks (10 ms each) via
 * sys_ticks; gettimeofday is built on top of that. No wall-clock today --
 * time() returns a constant value, which Doom only uses to seed its PRNG. */
#ifndef _STINK_TIME_H
#define _STINK_TIME_H

typedef long time_t;
typedef long suseconds_t;

struct timeval {
	time_t       tv_sec;
	suseconds_t  tv_usec;
};

struct timezone {
	int tz_minuteswest;
	int tz_dsttime;
};

int    gettimeofday(struct timeval *tv, struct timezone *tz);
time_t time(time_t *out);

#endif
