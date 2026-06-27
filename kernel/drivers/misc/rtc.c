/* CMOS RTC driver: reads the wall-clock date/time out of the 8042-era CMOS
 * chip via its index/data port pair. Values usually come back as BCD digits
 * (and the hour register can be 12-hour with a PM flag in bit 7), so this
 * normalizes everything to plain binary, 24-hour fields before returning. */
#include "rtc.h"
#include "io.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

#define CMOS_REG_SECOND       0x00
#define CMOS_REG_ALARM_SECOND 0x01
#define CMOS_REG_MINUTE       0x02
#define CMOS_REG_ALARM_MINUTE 0x03
#define CMOS_REG_HOUR         0x04
#define CMOS_REG_ALARM_HOUR   0x05
#define CMOS_REG_DAY          0x07
#define CMOS_REG_MONTH        0x08
#define CMOS_REG_YEAR         0x09
#define CMOS_REG_STATUSA      0x0A
#define CMOS_REG_STATUSB      0x0B
#define CMOS_REG_STATUSC      0x0C

/* Status B bit 5: alarm interrupt enable. */
#define CMOS_STATUSB_AIE      0x20

static unsigned char cmos_read(unsigned char reg)
{
	outb(CMOS_ADDR, reg);
	return inb(CMOS_DATA);
}

static int update_in_progress(void)
{
	return cmos_read(CMOS_REG_STATUSA) & 0x80;
}

static unsigned int bcd_to_bin(unsigned char v)
{
	return (unsigned int)((v >> 4) * 10 + (v & 0x0F));
}

struct raw_time {
	unsigned char second, minute, hour, day, month, year;
};

static void read_raw(struct raw_time *t)
{
	t->second = cmos_read(CMOS_REG_SECOND);
	t->minute = cmos_read(CMOS_REG_MINUTE);
	t->hour   = cmos_read(CMOS_REG_HOUR);
	t->day    = cmos_read(CMOS_REG_DAY);
	t->month  = cmos_read(CMOS_REG_MONTH);
	t->year   = cmos_read(CMOS_REG_YEAR);
}

static int raw_equal(const struct raw_time *a, const struct raw_time *b)
{
	return a->second == b->second && a->minute == b->minute &&
	       a->hour == b->hour && a->day == b->day &&
	       a->month == b->month && a->year == b->year;
}

void rtc_read(struct rtc_time *out)
{
	struct raw_time prev, cur;

	/* The chip updates its registers about once a second; reading mid
	 * update tears the value, so wait it out and keep re-reading until
	 * two consecutive snapshots agree. */
	while (update_in_progress())
		;
	read_raw(&prev);
	for (;;) {
		while (update_in_progress())
			;
		read_raw(&cur);
		if (raw_equal(&prev, &cur))
			break;
		prev = cur;
	}

	unsigned char status_b = cmos_read(CMOS_REG_STATUSB);
	int is_binary = status_b & 0x04;
	int is_24h = status_b & 0x02;

	unsigned int hour_is_pm = cur.hour & 0x80;
	cur.hour &= 0x7F;

	unsigned int second = is_binary ? cur.second : bcd_to_bin(cur.second);
	unsigned int minute = is_binary ? cur.minute : bcd_to_bin(cur.minute);
	unsigned int hour   = is_binary ? cur.hour   : bcd_to_bin(cur.hour);
	unsigned int day    = is_binary ? cur.day    : bcd_to_bin(cur.day);
	unsigned int month  = is_binary ? cur.month  : bcd_to_bin(cur.month);
	unsigned int year   = is_binary ? cur.year   : bcd_to_bin(cur.year);

	if (!is_24h) {
		hour %= 12;
		if (hour_is_pm)
			hour += 12;
	}

	out->second = second;
	out->minute = minute;
	out->hour = hour;
	out->day = day;
	out->month = month;
	out->year = 2000 + year;       /* the CMOS year register is 2 digits */
}

static void cmos_write(unsigned char reg, unsigned char val)
{
	outb(CMOS_ADDR, reg);
	outb(CMOS_DATA, val);
}

static unsigned char bin_to_bcd(unsigned int v)
{
	return (unsigned char)(((v / 10) << 4) | (v % 10));
}

/* Set on every IRQ8 fire, cleared by rtc_alarm_fired() consumer. The
 * IRQ races against the consumer but the read+clear pattern below is
 * naturally one-shot: at most one fire is reported per consumer call. */
static volatile int alarm_pending;

int rtc_set_alarm(unsigned int hour, unsigned int minute, unsigned int second)
{
	if (hour > 23 || minute > 59 || second > 59)
		return -1;

	/* Convert to CMOS register format -- match the running format the
	 * RTC was already in (BCD vs binary). Status B bit 2 tells us. */
	unsigned char status_b = cmos_read(CMOS_REG_STATUSB);
	int binary_mode = status_b & 0x04;

	unsigned char h = binary_mode ? (unsigned char)hour   : bin_to_bcd(hour);
	unsigned char m = binary_mode ? (unsigned char)minute : bin_to_bcd(minute);
	unsigned char s = binary_mode ? (unsigned char)second : bin_to_bcd(second);

	cmos_write(CMOS_REG_ALARM_HOUR,   h);
	cmos_write(CMOS_REG_ALARM_MINUTE, m);
	cmos_write(CMOS_REG_ALARM_SECOND, s);

	/* Re-read status B and set AIE; writing status B from a stale snapshot
	 * could clobber other config bits, so we read again right before. */
	status_b = cmos_read(CMOS_REG_STATUSB);
	cmos_write(CMOS_REG_STATUSB, status_b | CMOS_STATUSB_AIE);
	alarm_pending = 0;
	return 0;
}

void rtc_clear_alarm(void)
{
	unsigned char status_b = cmos_read(CMOS_REG_STATUSB);
	cmos_write(CMOS_REG_STATUSB, status_b & (unsigned char)~CMOS_STATUSB_AIE);
}

int rtc_alarm_fired(void)
{
	int fired = alarm_pending;
	alarm_pending = 0;
	return fired;
}

void rtc_handle_irq(void)
{
	/* Reading status C is mandatory after an RTC interrupt: it clears the
	 * source bits inside the chip so further interrupts can fire. The bits
	 * tell us what kind (alarm vs periodic vs update-ended); for the
	 * alarm-only setup we treat any RTC IRQ as "the alarm". */
	(void)cmos_read(CMOS_REG_STATUSC);
	alarm_pending = 1;
}
