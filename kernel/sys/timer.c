/* One-shot kernel timer table. See timer.h for the contract. */
#include "defs.h"
#include "timer.h"

struct timer_slot {
	int            in_use;
	unsigned int   deadline_ticks;    /* fire when pit_ticks() >= this */
	timer_cb_t     cb;
	void          *arg;
};

static struct timer_slot table[TIMER_MAX];

void timer_init(void)
{
	for (int i = 0; i < TIMER_MAX; i++)
		table[i].in_use = 0;
}

int timer_add(unsigned int delay_ms, timer_cb_t cb, void *arg)
{
	if (!cb)
		return -1;

	/* PIT runs at 100 Hz (10 ms per tick). Round delay UP so a request for
	 * any non-zero ms always waits at least one tick -- matches SYS_SLEEP. */
	unsigned int delta = (delay_ms + 9) / 10;
	if (delta == 0)
		delta = 1;

	for (int i = 0; i < TIMER_MAX; i++) {
		if (table[i].in_use)
			continue;
		table[i].in_use         = 1;
		table[i].deadline_ticks = pit_ticks() + delta;
		table[i].cb             = cb;
		table[i].arg            = arg;
		return i + 1;                    /* handles start at 1, 0 reserved */
	}
	return -1;                                /* table full */
}

int timer_cancel(int handle)
{
	int idx = handle - 1;
	if (idx < 0 || idx >= TIMER_MAX)
		return -1;
	table[idx].in_use = 0;
	return 0;
}

void timer_tick(void)
{
	unsigned int now = pit_ticks();
	for (int i = 0; i < TIMER_MAX; i++) {
		struct timer_slot *t = &table[i];
		if (!t->in_use)
			continue;
		/* The subtraction comparison handles tick wraparound correctly: a
		 * deadline of (now - 1) is "expired" not "very far in the future". */
		if ((int)(now - t->deadline_ticks) < 0)
			continue;
		timer_cb_t cb = t->cb;
		void *arg     = t->arg;
		t->in_use     = 0;                /* free before firing so the cb */
		                                  /* can re-arm into the same slot */
		cb(arg);
	}
}

int timer_active_count(void)
{
	int n = 0;
	for (int i = 0; i < TIMER_MAX; i++)
		if (table[i].in_use)
			n++;
	return n;
}
