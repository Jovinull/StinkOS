/* StinkOS userland app written in C, using the libstink syscall wrappers.
 * Logs a line, draws a cyan square, waits for a key, then returns (crt0 exits). */
#include "libstink.h"

void main(void)
{
	sys_log("hi from c app");

	for (int y = 0; y < 20; y++)
		for (int x = 0; x < 20; x++)
			sys_draw(200 + x, 50 + y, 0x00FFFF);   /* cyan square */

	while (sys_getkey() == 0)
		;                                          /* idle until a key */
}
