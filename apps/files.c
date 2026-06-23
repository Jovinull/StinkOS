/* StinkOS userland C app: exercises the StinkFS named-file syscalls. Writes part
 * of a string to "note.txt", appends the rest, then reads the whole file back
 * and logs it -- proving create/write/append/read all round-trip through disk. */
#include "libstink.h"

void main(void)
{
	char buf[32];

	if (sys_fwrite("note.txt", "stinkfs ", 8) != 0) {
		sys_log("files: write failed");
	} else if (sys_fappend("note.txt", "ok", 2) != 0) {
		sys_log("files: append failed");
	} else {
		int n = sys_fread("note.txt", buf, sizeof(buf) - 1);
		if (n < 0) {
			sys_log("files: read failed");
		} else {
			buf[n] = 0;
			sys_log(buf);                  /* -> "ring3: stinkfs ok" */
			sys_log("files: read back ok");
		}
	}

	while (sys_getkey() == 0)
		;
}
