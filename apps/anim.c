/* StinkOS userland C app: a time-driven animation using the PIT tick counter
 * (SYS_TICKS). A magenta dot steps right one position per timer tick, proving
 * the kernel exposes time to userland. Press any key to return to the menu. */
#include "libstink.h"

#define OY 34

void main(void)
{
	sys_log("anim app running");

	draw_window_frame(0, 0, 1024, 768,
	    "Tick Animation  --  press any key to exit");
	sys_fillrect(0, OY, 1024, 768 - OY, 0x000000);

	unsigned int t0 = sys_ticks();
	while (sys_ticks() == t0)
		;                                  /* wait for the clock to advance */
	sys_log("anim: time flows");

	for (int frame = 0; frame < 40; frame++) {
		unsigned int t = sys_ticks();
		while (sys_ticks() == t)
			;                          /* pace one frame per tick */
		int x = 100 + frame * 8;
		for (int dy = 0; dy < 6; dy++)
			for (int dx = 0; dx < 6; dx++)
				sys_draw(x + dx, OY + 200 + dy, 0xFF00FF);
		if (sys_getkey() != 0)
			return;
	}

	while (sys_getkey() == 0)
		;
}
