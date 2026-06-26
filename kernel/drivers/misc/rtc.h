/* CMOS real-time clock (the same chip that keeps the wall clock ticking
 * while the machine is off). Reading it is a privileged port I/O operation
 * (ports 0x70/0x71), so only kernel-side code (e.g. menu.c) can call this --
 * a ring-3 app can't touch it directly.
 *
 * NOT YET LINKED INTO THE KERNEL: rtc.c isn't in the Makefile's C_SRCS yet.
 * See the pending request in TASKS.md. */
#ifndef RTC_H
#define RTC_H

struct rtc_time {
	unsigned int year;    /* e.g. 2026 */
	unsigned int month;   /* 1-12 */
	unsigned int day;     /* 1-31 */
	unsigned int hour;    /* 0-23 */
	unsigned int minute;  /* 0-59 */
	unsigned int second;  /* 0-59 */
};

/* Blocks briefly (spins on the RTC's "update in progress" flag) and fills
 * *out with the current wall-clock time. */
void rtc_read(struct rtc_time *out);

#endif
