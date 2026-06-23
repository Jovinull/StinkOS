/* StinkOS userland C app: exercises offset reads (SYS_FREAD_AT). Writes a known
 * string to a file, then reads a 4-byte slice from the middle and checks it. */
#include "libstink.h"

void main(void)
{
	static const char data[] = "0123456789";
	static const char want[] = "3456";
	char buf[8];

	sys_fwrite("seek.txt", data, 10);

	int n = sys_fread_at("seek.txt", buf, 4, 3);   /* bytes 3..6 -> "3456" */
	int ok = (n == 4);
	for (int i = 0; i < 4 && ok; i++)
		if (buf[i] != want[i])
			ok = 0;

	if (ok) {
		buf[n] = 0;
		sys_log(buf);                          /* -> "ring3: 3456" */
		sys_log("seek: offset read ok");
	} else {
		sys_log("seek: offset read failed");
	}

	while (sys_getkey() == 0)
		;
}
