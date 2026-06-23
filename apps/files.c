/* StinkOS userland C app: exercises the StinkFS named-file syscalls. Writes a
 * string to "note.txt", reads it back into a local buffer and logs the result,
 * proving a full create/write/read round-trip through the on-disk filesystem. */
#include "libstink.h"

void main(void)
{
	static const char msg[] = "stinkfs ok";
	char buf[32];

	unsigned int len = 0;
	while (msg[len])
		len++;

	if (sys_fwrite("note.txt", msg, len) != 0) {
		sys_log("files: write failed");
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
