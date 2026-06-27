/* Host-side test for the round-robin scheduler's pick logic
 * (`next_ready` in kernel/sys/proc.c). The scheduler picks the lowest
 * priority value among PROC_READY slots; equal-priority procs round-robin
 * fairly from `from + 1`. Algorithm replica kept in sync by hand.
 */
#include <stdio.h>

#define PROC_MAX  16

enum { UNUSED = 0, EMBRYO, READY, RUNNING, SLEEPING, ZOMBIE };

struct proc {
	int state;
	int priority;
};

static struct proc table[PROC_MAX];

static int next_ready_idx(int from)
{
	int best_prio = 32;
	int best_idx  = -1;
	for (int i = 1; i <= PROC_MAX; i++) {
		int idx = (from + i) % PROC_MAX;
		if (table[idx].state != READY) continue;
		if (table[idx].priority < best_prio) {
			best_prio = table[idx].priority;
			best_idx  = idx;
		}
	}
	return best_idx;
}

static int expect_eq(const char *label, int got, int want)
{
	if (got == want) {
		printf("ok   %s = %d\n", label, got);
		return 0;
	}
	printf("FAIL %s: got %d, want %d\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;

	/* Empty table: no READY, no winner. */
	for (int i = 0; i < PROC_MAX; i++) table[i].state = UNUSED;
	failures += expect_eq("empty table", next_ready_idx(0), -1);

	/* One READY slot wins regardless of `from`. */
	for (int i = 0; i < PROC_MAX; i++) table[i].state = UNUSED;
	table[3].state = READY; table[3].priority = 16;
	failures += expect_eq("only slot 3 ready (from 0)", next_ready_idx(0), 3);
	failures += expect_eq("only slot 3 ready (from 7)", next_ready_idx(7), 3);

	/* Two slots, same priority: round-robin from `from + 1`. */
	for (int i = 0; i < PROC_MAX; i++) table[i].state = UNUSED;
	table[2].state = READY; table[2].priority = 10;
	table[5].state = READY; table[5].priority = 10;
	failures += expect_eq("tie from 0 -> 2", next_ready_idx(0), 2);
	failures += expect_eq("tie from 2 -> 5", next_ready_idx(2), 5);
	failures += expect_eq("tie from 5 -> 2", next_ready_idx(5), 2);

	/* Mixed priorities: lower value wins regardless of position. */
	for (int i = 0; i < PROC_MAX; i++) table[i].state = UNUSED;
	table[1].state  = READY; table[1].priority  = 20;
	table[9].state  = READY; table[9].priority  = 5;
	table[12].state = READY; table[12].priority = 15;
	failures += expect_eq("highest prio wins (from 0)",  next_ready_idx(0),  9);
	failures += expect_eq("highest prio wins (from 10)", next_ready_idx(10), 9);

	/* SLEEPING / ZOMBIE / EMBRYO slots are ignored even if priority is low. */
	for (int i = 0; i < PROC_MAX; i++) table[i].state = UNUSED;
	table[4].state = SLEEPING; table[4].priority = 0;   /* highest prio but sleeping */
	table[6].state = ZOMBIE;   table[6].priority = 1;
	table[8].state = EMBRYO;   table[8].priority = 2;
	table[11].state = READY;   table[11].priority = 16;
	failures += expect_eq("non-READY ignored", next_ready_idx(0), 11);

	/* Tie between three slots: round-robin walks forward from `from`. */
	for (int i = 0; i < PROC_MAX; i++) table[i].state = UNUSED;
	table[0].state  = READY; table[0].priority  = 8;
	table[5].state  = READY; table[5].priority  = 8;
	table[10].state = READY; table[10].priority = 8;
	failures += expect_eq("triple tie from 0  -> 5",  next_ready_idx(0),  5);
	failures += expect_eq("triple tie from 5  -> 10", next_ready_idx(5),  10);
	failures += expect_eq("triple tie from 10 -> 0",  next_ready_idx(10), 0);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
