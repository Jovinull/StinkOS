/* Host-side test for the RTC alarm helpers in kernel/drivers/misc/rtc.c
 * (rtc_set_alarm + rtc_alarm_fired). Two pieces:
 *
 *   1. bin_to_bcd: pack a 0..59 value into a CMOS BCD byte (high nibble
 *      = tens digit, low nibble = ones digit). The CMOS chip uses BCD
 *      whenever status B bit 2 is CLEAR, which is the BIOS default; we
 *      have to honour that or the alarm will fire at the wrong time.
 *
 *   2. The h/m/s range validator: rtc_set_alarm returns -1 if any
 *      component is out of range (h > 23, m > 59, s > 59). Otherwise 0.
 *      Wrong-range inputs must NOT touch the CMOS registers.
 *
 * 3. alarm_pending one-shot semantics: rtc_alarm_fired returns 1 the
 *    first time after an IRQ and 0 thereafter until the next IRQ.
 */
#include <stdio.h>

static unsigned char bin_to_bcd(unsigned int v)
{
	return (unsigned char)(((v / 10) << 4) | (v % 10));
}

static int range_ok(unsigned int h, unsigned int m, unsigned int s)
{
	if (h > 23 || m > 59 || s > 59) return 0;
	return 1;
}

/* Mirror of alarm_pending consumer side. */
static volatile int alarm_pending;
static int alarm_fired(void)
{
	int f = alarm_pending;
	alarm_pending = 0;
	return f;
}

static int expect_uint(const char *label, unsigned int got, unsigned int want)
{
	if (got == want) { printf("ok   %-50s = %u\n", label, got); return 0; }
	printf("FAIL %s: got %u, want %u\n", label, got, want);
	return 1;
}

static int expect_int(const char *label, int got, int want)
{
	if (got == want) { printf("ok   %-50s = %d\n", label, got); return 0; }
	printf("FAIL %s: got %d, want %d\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;

	/* --- BCD encoding -------------------------------------------------- */
	failures += expect_uint("bcd(0) = 0x00",       bin_to_bcd(0),  0x00);
	failures += expect_uint("bcd(9) = 0x09",       bin_to_bcd(9),  0x09);
	failures += expect_uint("bcd(10) = 0x10",      bin_to_bcd(10), 0x10);
	failures += expect_uint("bcd(12) = 0x12",      bin_to_bcd(12), 0x12);
	failures += expect_uint("bcd(23) = 0x23",      bin_to_bcd(23), 0x23);
	failures += expect_uint("bcd(45) = 0x45",      bin_to_bcd(45), 0x45);
	failures += expect_uint("bcd(59) = 0x59",      bin_to_bcd(59), 0x59);

	/* --- Range validator ---------------------------------------------- */
	failures += expect_int("00:00:00 accepted",        range_ok(0, 0, 0),       1);
	failures += expect_int("23:59:59 accepted",        range_ok(23, 59, 59),    1);
	failures += expect_int("12:34:56 accepted",        range_ok(12, 34, 56),    1);
	failures += expect_int("24:00:00 rejected (h>23)", range_ok(24, 0, 0),      0);
	failures += expect_int("00:60:00 rejected (m>59)", range_ok(0, 60, 0),      0);
	failures += expect_int("00:00:60 rejected (s>59)", range_ok(0, 0, 60),      0);
	failures += expect_int("999:0:0 rejected",         range_ok(999, 0, 0),     0);

	/* --- alarm_fired one-shot ----------------------------------------- */
	alarm_pending = 0;
	failures += expect_int("idle: not fired",       alarm_fired(), 0);
	alarm_pending = 1;                              /* IRQ would set this */
	failures += expect_int("after IRQ: fired = 1",  alarm_fired(), 1);
	failures += expect_int("after consume: 0",      alarm_fired(), 0);
	failures += expect_int("repeat consume: 0",     alarm_fired(), 0);
	alarm_pending = 1;                              /* second IRQ */
	failures += expect_int("after 2nd IRQ: 1",      alarm_fired(), 1);
	failures += expect_int("after 2nd consume: 0",  alarm_fired(), 0);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
