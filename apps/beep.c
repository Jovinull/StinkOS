/* StinkOS userland C app: plays a short three-note rising arpeggio on the PC
 * speaker through the SYS_SOUND syscall, pacing each note with the PIT tick
 * clock (SYS_TICKS). Silences the speaker, then waits for a key to exit. */
#include "libstink.h"

static void delay(unsigned int ticks)
{
	unsigned int t0 = sys_ticks();
	while (sys_ticks() - t0 < ticks)
		;                                  /* busy-wait on the timer */
}

void main(void)
{
	sys_log("beep: start");

	static const unsigned int notes[3] = { 523, 659, 784 };   /* C5 E5 G5 */
	for (int i = 0; i < 3; i++) {
		sys_sound(notes[i]);
		delay(15);                         /* ~150 ms per note at 100 Hz */
	}
	sys_sound(0);                              /* silence */

	sys_log("beep: done");

	while (sys_getkey() == 0)
		;                                  /* hold until a key, then exit */
}
