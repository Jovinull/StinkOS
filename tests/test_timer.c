/* Host-side test for the one-shot kernel timer table in
 * kernel/sys/timer.c. Pure data flow except for `pit_ticks()`, which
 * we mock with a settable `now_ticks` counter. Contract:
 *   - handles start at 1 (0 reserved)
 *   - delay_ms rounds UP, sub-tick requests still wait one tick
 *   - cb fires when now >= deadline (signed subtract handles
 *     32-bit wraparound)
 *   - slot freed BEFORE the cb is called so the cb may re-arm into
 *     the same slot
 *   - cancel of unknown handle -> -1; valid handle -> 0
 *
 * If the wraparound test passes, an `if (now < deadline) skip`
 * regression -- the most obvious "fix" -- gets caught.
 */
#include <stdio.h>
#include <stdint.h>

#define TIMER_MAX 16

typedef void (*timer_cb_t)(void *arg);

struct slot {
	int          in_use;
	unsigned int deadline_ticks;
	timer_cb_t   cb;
	void        *arg;
};

static struct slot   table[TIMER_MAX];
static unsigned int  now_ticks;

static unsigned int pit_ticks(void) { return now_ticks; }

static void timer_init(void)
{
	for (int i = 0; i < TIMER_MAX; i++) table[i].in_use = 0;
}

static int timer_add(unsigned int delay_ms, timer_cb_t cb, void *arg)
{
	if (!cb) return -1;
	unsigned int delta = (delay_ms + 9) / 10;
	if (delta == 0) delta = 1;
	for (int i = 0; i < TIMER_MAX; i++) {
		if (table[i].in_use) continue;
		table[i].in_use         = 1;
		table[i].deadline_ticks = pit_ticks() + delta;
		table[i].cb             = cb;
		table[i].arg            = arg;
		return i + 1;
	}
	return -1;
}

static int timer_cancel(int handle)
{
	int idx = handle - 1;
	if (idx < 0 || idx >= TIMER_MAX) return -1;
	table[idx].in_use = 0;
	return 0;
}

static void timer_tick(void)
{
	unsigned int now = pit_ticks();
	for (int i = 0; i < TIMER_MAX; i++) {
		struct slot *t = &table[i];
		if (!t->in_use) continue;
		if ((int)(now - t->deadline_ticks) < 0) continue;
		timer_cb_t cb = t->cb;
		void *arg     = t->arg;
		t->in_use = 0;
		cb(arg);
	}
}

static int timer_active_count(void)
{
	int n = 0;
	for (int i = 0; i < TIMER_MAX; i++) if (table[i].in_use) n++;
	return n;
}

/* --- test callbacks --- */
static int fire_count;
static int last_arg;
static void inc_cb(void *arg) { fire_count++; last_arg = (int)(uintptr_t)arg; }

/* Re-arm callback: re-adds itself with a fresh delay. */
static int rearm_count;
static void rearm_cb(void *arg)
{
	rearm_count++;
	if (rearm_count < 3)
		(void)timer_add(10, rearm_cb, arg);
}

static int expect_int(const char *label, int got, int want)
{
	if (got == want) { printf("ok   %-55s = %d\n", label, got); return 0; }
	printf("FAIL %s: got %d, want %d\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;
	int h;

	/* --- handle numbering: first add -> 1. ------------------------- */
	timer_init();
	now_ticks = 100;
	h = timer_add(50, inc_cb, (void *)1);
	failures += expect_int("first add -> handle 1",  h, 1);
	failures += expect_int("active = 1",             timer_active_count(), 1);

	/* --- NULL cb rejected. ---------------------------------------- */
	failures += expect_int("add NULL cb -> -1",
	                       timer_add(50, 0, 0), -1);

	/* --- delay round-up: 5ms (sub-tick) still waits 1 tick. ------ */
	timer_init();
	now_ticks = 1000;
	h = timer_add(5, inc_cb, (void *)1);
	failures += expect_int("delay 5ms rounds up to 1 tick",
	                       (int)(table[h - 1].deadline_ticks - 1000), 1);
	h = timer_add(0, inc_cb, (void *)1);          /* 0ms also waits 1 tick */
	failures += expect_int("delay 0ms still 1 tick",
	                       (int)(table[h - 1].deadline_ticks - 1000), 1);
	h = timer_add(50, inc_cb, (void *)1);
	failures += expect_int("delay 50ms = 5 ticks",
	                       (int)(table[h - 1].deadline_ticks - 1000), 5);

	/* --- fire on tick: pre-deadline -> no fire, at/after -> fire. */
	timer_init();
	fire_count = 0;
	last_arg   = 0;
	now_ticks  = 100;
	(void)timer_add(50, inc_cb, (void *)42);     /* deadline = 105 */
	now_ticks = 104;
	timer_tick();
	failures += expect_int("now < deadline: no fire", fire_count, 0);
	now_ticks = 105;
	timer_tick();
	failures += expect_int("now == deadline: fires", fire_count, 1);
	failures += expect_int("cb arg passthrough",     last_arg,   42);
	failures += expect_int("post-fire slot freed",   timer_active_count(), 0);

	/* --- cancel: handle invalidates the slot, no fire. ------------ */
	timer_init();
	fire_count = 0;
	now_ticks  = 0;
	h = timer_add(50, inc_cb, (void *)1);
	failures += expect_int("cancel valid -> 0",   timer_cancel(h), 0);
	failures += expect_int("post-cancel inactive", timer_active_count(), 0);
	now_ticks = 1000;
	timer_tick();
	failures += expect_int("post-cancel no fire",  fire_count, 0);

	/* Cancel of out-of-range handle: -1. */
	failures += expect_int("cancel 0   -> -1", timer_cancel(0),   -1);
	failures += expect_int("cancel 99  -> -1", timer_cancel(99),  -1);

	/* --- table exhaustion ----------------------------------------- */
	timer_init();
	for (int i = 0; i < TIMER_MAX; i++) (void)timer_add(50, inc_cb, 0);
	failures += expect_int("table full -> -1",
	                       timer_add(50, inc_cb, 0), -1);

	/* --- re-arm during cb: the cb adds another timer using the same
	 *     slot that was just freed. With re-arm bumping rearm_count
	 *     to 3, we should see 3 fires across 3 ticks. ------------- */
	timer_init();
	rearm_count = 0;
	now_ticks   = 0;
	(void)timer_add(10, rearm_cb, 0);
	for (int t = 0; t < 5; t++) {
		now_ticks += 1;
		timer_tick();
	}
	failures += expect_int("re-arm: fired 3 times",   rearm_count, 3);
	failures += expect_int("re-arm: no slot leak",    timer_active_count(), 0);

	/* --- wraparound: deadline just before 0xFFFFFFFF, now wraps to
	 *     0; signed subtract sees `now - deadline` as positive small,
	 *     fires correctly. -------------------------------------- */
	timer_init();
	fire_count = 0;
	now_ticks  = 0xFFFFFFF0u;
	(void)timer_add(50, inc_cb, 0);              /* delta = 5 ticks
	                                              * deadline = 0xFFFFFFF5 */
	now_ticks  = 0xFFFFFFF4u;                    /* one tick early */
	timer_tick();
	failures += expect_int("wrap: pre-deadline no fire",  fire_count, 0);
	now_ticks  = 0xFFFFFFF5u;                    /* exactly the deadline */
	timer_tick();
	failures += expect_int("wrap: at deadline fires",     fire_count, 1);

	/* Schedule one near the very top and have it fire after the
	 * counter wraps past zero. */
	timer_init();
	fire_count = 0;
	now_ticks  = 0xFFFFFFF0u;
	(void)timer_add(200, inc_cb, 0);             /* delta = 20 -> deadline 4 */
	now_ticks  = 0xFFFFFFFFu;
	timer_tick();
	failures += expect_int("post-wrap pre-deadline: no fire", fire_count, 0);
	now_ticks  = 4u;                              /* tick counter has wrapped */
	timer_tick();
	failures += expect_int("post-wrap at deadline: fires",    fire_count, 1);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
