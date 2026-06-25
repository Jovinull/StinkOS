/* StinkOS userland C app: a minimal interactive command shell. Reads a line
 * from the keyboard (with backspace editing), parses a command + argument(s),
 * and drives StinkFS through the existing file syscalls. Output goes to the
 * serial log via sys_log/sys_printf -- there is no text-on-screen syscall yet,
 * so this is a serial console for now, not a graphical terminal.
 *
 * NOT YET ON THE BOOT MENU: needs a disk slot + TOC entry in the Makefile,
 * which is Core's (Equipe 1's) jurisdiction. See the pending request in
 * TASKS.md. */
#include "libstink.h"

#define LINE_MAX 64

static int read_line(char *buf)
{
	int len = 0;

	for (;;) {
		int c = sys_getkey();
		if (c == 0)
			continue;               /* busy-poll: no yield/sleep syscall */
		if (c == '\n') {
			buf[len] = '\0';
			return len;
		}
		if (c == '\b') {
			if (len > 0)
				len--;
			continue;
		}
		if (len < LINE_MAX - 1)
			buf[len++] = (char)c;
	}
}

/* Splits 'line' at the first space, NUL-terminating the command word and
 * returning a pointer to whatever comes after it (or "" if there's none). */
static char *split(char *line)
{
	for (char *p = line; *p != '\0'; p++) {
		if (*p == ' ') {
			*p = '\0';
			return p + 1;
		}
	}
	return line + strlen(line);
}

void main(void)
{
	sys_log("shell: ready");

	for (;;) {
		char line[LINE_MAX];
		read_line(line);

		char *rest = split(line);

		if (strcmp(line, "") == 0) {
			continue;
		} else if (strcmp(line, "help") == 0) {
			sys_log("shell: commands: help ls cat write rm exit");
		} else if (strcmp(line, "exit") == 0) {
			sys_log("shell: bye");
			sys_exit();
		} else if (strcmp(line, "ls") == 0) {
			int n = sys_fcount();
			char name[16];
			for (int i = 0; i < n; i++)
				if (sys_finfo(i, name) >= 0) {
					name[15] = '\0';
					sys_log(name);
				}
			sys_printf("shell: %d file(s)", n);
		} else if (strcmp(line, "cat") == 0) {
			char data[128];
			int n = sys_fread(rest, data, sizeof(data) - 1);
			if (n < 0) {
				sys_log("shell: no such file");
			} else {
				data[n] = '\0';
				sys_log(data);
			}
		} else if (strcmp(line, "rm") == 0) {
			if (sys_fdelete(rest) == 0)
				sys_log("shell: removed");
			else
				sys_log("shell: no such file");
		} else if (strcmp(line, "write") == 0) {
			char *text = split(rest);
			if (sys_fwrite(rest, text, strlen(text)) == 0)
				sys_log("shell: written");
			else
				sys_log("shell: write failed");
		} else {
			sys_printf("shell: unknown command: %s", line);
		}
	}
}
