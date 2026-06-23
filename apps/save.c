/* StinkOS userland C app: demonstrates persistence using an ordinary StinkFS
 * file. Reads a counter from "counter", increments it, writes it back, then
 * re-reads it to prove the value reached the disk. Each launch counts up. */
#include "libstink.h"

void main(void)
{
	unsigned int v = 0;
	char buf[4];

	if (sys_fread("counter", buf, 4) == 4)
		v = *(unsigned int *)buf;

	v = v + 1;
	*(unsigned int *)buf = v;
	sys_fwrite("counter", buf, 4);

	if (sys_fread("counter", buf, 4) == 4 && *(unsigned int *)buf == v)
		sys_log("save: persisted ok");
	else
		sys_log("save: persist failed");

	while (sys_getkey() == 0)
		;
}
