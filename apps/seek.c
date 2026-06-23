/* StinkOS userland C app: exercises offset I/O (SYS_FREAD_AT / SYS_FWRITE_AT).
 * Writes a known string, reads a middle slice, then overwrites two bytes at an
 * offset and reads them back to confirm in-place writes land where expected. */
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

	/* overwrite bytes 4..5 with "XY" -> file becomes "0123XY6789" */
	sys_fwrite_at("seek.txt", "XY", 2, 4);
	int m = sys_fread_at("seek.txt", buf, 4, 3);   /* bytes 3..6 -> "3XY6" */
	int ok2 = (m == 4 && buf[0] == '3' && buf[1] == 'X' &&
	           buf[2] == 'Y' && buf[3] == '6');
	if (ok2)
		sys_log("seek: offset write ok");
	else
		sys_log("seek: offset write failed");

	while (sys_getkey() == 0)
		;
}
