/* One-shot kernel timer subsystem.
 *
 * Sits on top of the PIT IRQ tick (100 Hz). A caller registers a callback +
 * a delay in milliseconds; the subsystem fires the callback exactly once
 * when the deadline expires, then frees the slot. Callbacks run in IRQ
 * context -- keep them short, never block, never touch user memory.
 *
 * Until this primitive existed, code that needed "do X after Y ms" had to
 * either busy-wait on pit_ticks() (blocks the CPU) or piggyback on
 * net_poll_once() (only fires when something pumps the network). Neither
 * pattern scales beyond a couple of consumers.
 */
#ifndef TIMER_H
#define TIMER_H

#define TIMER_MAX  16

typedef void (*timer_cb_t)(void *arg);

/* Initialise the timer table (call once during boot, after pit_init).      */
void timer_init(void);

/* Schedule `cb(arg)` to fire once `delay_ms` from now. Returns a positive
 * handle on success, -1 if the table is full or arguments are bad.        */
int  timer_add(unsigned int delay_ms, timer_cb_t cb, void *arg);

/* Cancel a pending timer. Safe to call on a fired or already-cancelled
 * handle; returns 0 either way. -1 only if the handle is out of range.   */
int  timer_cancel(int handle);

/* Drive the table: called from the PIT IRQ handler. Walks every active
 * slot, fires any whose deadline has expired, and releases their slots. */
void timer_tick(void);

/* Number of pending (not-yet-fired) timers. For debugging / introspection. */
int  timer_active_count(void);

#endif
