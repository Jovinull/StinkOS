/* StinkOS userland C app: lists the files stored in StinkFS. Asks the kernel how
 * many files exist (SYS_FCOUNT) and logs each name (SYS_FINFO), the userland
 * equivalent of an "ls". Press a key to return to the menu. */
#include "libstink.h"

void main(void)
{
	int n = sys_fcount();
	char name[16];

	for (int i = 0; i < n; i++) {
		if (sys_finfo(i, name) >= 0) {
			name[15] = 0;          /* guarantee termination */
			sys_log(name);         /* one log line per file */
		}
	}

	if (n > 0)
		sys_log("ls: found files");
	sys_log("ls: done");

	while (sys_getkey() == 0)
		;
}
