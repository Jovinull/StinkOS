/* StinkOS userland C app: exercises StinkFS deletion and data compaction.
 * Creates two files, deletes the first, then re-reads the second -- whose data
 * was slid down over the hole -- and checks it is still intact. */
#include "libstink.h"

void main(void)
{
	static const char a[] = "AAAA";
	static const char b[] = "BBBBBB";
	char buf[8];

	sys_fwrite("a.txt", a, 4);
	sys_fwrite("b.txt", b, 6);

	sys_fdelete("a.txt");              /* b.txt's data must shift down a sector */

	int n = sys_fread("b.txt", buf, sizeof(buf) - 1);
	int ok = (n == 6);
	for (int i = 0; i < 6 && ok; i++)
		if (buf[i] != b[i])
			ok = 0;

	if (ok)
		sys_log("del: compaction ok");
	else
		sys_log("del: compaction failed");

	while (sys_getkey() == 0)
		;
}
