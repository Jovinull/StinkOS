/* CMOS real-time clock (the same chip that keeps the wall clock ticking
 * while the machine is off). Reading it is a privileged port I/O operation
 * (ports 0x70/0x71), so only kernel-side code can call this directly --
 * ring-3 apps reach it via SYS_RTC_READ / SYS_RTC_SET_ALARM. */
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

/* Program the CMOS RTC's alarm registers to fire IRQ8 at the next
 * occurrence of the given wall-clock time-of-day. Sets the AIE (alarm
 * interrupt enable) bit in status register B. Day/month/year are NOT
 * matched -- only h/m/s -- so the alarm fires at most once per day at
 * the chosen instant. Without an ACPI sleep state this is not a true
 * "wake from sleep", but apps can poll rtc_alarm_fired() to detect the
 * tick. Returns 0 on success, -1 if any field is out of range. */
int  rtc_set_alarm(unsigned int hour, unsigned int minute, unsigned int second);

/* Disarm the alarm (clears the AIE bit). The pending fire flag stays
 * set until rtc_alarm_fired() is called and observes 1. */
void rtc_clear_alarm(void);

/* Returns 1 (and clears the flag) if the alarm fired since the last
 * call; 0 otherwise. Single consumer; not safe across two pollers. */
int  rtc_alarm_fired(void);

/* Called from the IRQ8 dispatcher in trap.c -- acks the CMOS chip
 * (reading register C is mandatory to re-enable further interrupts)
 * and bumps the pending flag. */
void rtc_handle_irq(void);

#endif
