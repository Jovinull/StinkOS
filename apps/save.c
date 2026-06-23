/* StinkOS userland C app: demonstrates persistent storage. Loads a counter from
 * disk (SYS_LOAD), increments it, writes it back (SYS_SAVE), then loads it again
 * to prove the value actually reached the medium. Press a key to return. */
#include "libstink.h"

void main(void)
{
	unsigned int v = sys_load();
	v = v + 1;
	sys_save(v);

	if (sys_load() == v)               /* re-read from disk: persisted? */
		sys_log("save: persisted ok");
	else
		sys_log("save: persist failed");

	while (sys_getkey() == 0)
		;
}
