/* StinkOS userland C app: exercises the VFS file-descriptor syscalls. Opens
 * (creating) a file, writes through the descriptor, rewinds with seek, reads it
 * back and checks the bytes, then closes -- a full open/write/seek/read/close. */
#include "libstink.h"

void main(void)
{
	static const char msg[] = "hello-vfs";
	char buf[16];

	int fd = sys_open("fd.txt", SYS_O_CREATE);
	if (fd < 0) {
		sys_log("fd: open failed");
	} else {
		sys_write(fd, msg, 9);
		sys_seek(fd, 0, SYS_SEEK_SET);
		int n = sys_read(fd, buf, sizeof(buf) - 1);

		int ok = (n == 9);
		for (int i = 0; i < 9 && ok; i++)
			if (buf[i] != msg[i])
				ok = 0;

		if (ok) {
			buf[n] = 0;
			sys_log(buf);                  /* -> "ring3: hello-vfs" */
			sys_log("fd: rw ok");
		} else {
			sys_log("fd: rw failed");
		}
		sys_close(fd);
	}

	while (sys_getkey() == 0)
		;
}
