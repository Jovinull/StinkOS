/* CMOS RTC driver: reads the wall-clock date/time out of the 8042-era CMOS
 * chip via its index/data port pair. Values usually come back as BCD digits
 * (and the hour register can be 12-hour with a PM flag in bit 7), so this
 * normalizes everything to plain binary, 24-hour fields before returning. */
#include "rtc.h"
#include "io.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

#define CMOS_REG_SECOND  0x00
#define CMOS_REG_MINUTE  0x02
#define CMOS_REG_HOUR    0x04
#define CMOS_REG_DAY     0x07
#define CMOS_REG_MONTH   0x08
#define CMOS_REG_YEAR    0x09
#define CMOS_REG_STATUSA 0x0A
#define CMOS_REG_STATUSB 0x0B

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
