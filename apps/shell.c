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
#define HISTORY_MAX 8

/* A small ring of past command lines. history_pos counts how far back the
 * arrow keys have currently scrolled (0 = not recalling anything, editing a
 * fresh line). */
static char history[HISTORY_MAX][LINE_MAX];
static int history_count;
static int history_pos;

static void history_push(const char *line)
{
	if (line[0] == '\0')
		return;

	unsigned int n = strlen(line);
	if (n > LINE_MAX - 1)
		n = LINE_MAX - 1;

	if (history_count == HISTORY_MAX) {
		for (int i = 1; i < HISTORY_MAX; i++)
			memcpy(history[i - 1], history[i], LINE_MAX);
		history_count--;
	}
	memcpy(history[history_count], line, n);
	history[history_count][n] = '\0';
	history_count++;
}

/* Loads the recalled line (history_pos steps back from the newest, or the
 * empty string at history_pos == 0) into buf and returns its length. */
static int history_load(char *buf)
{
	if (history_pos == 0) {
		buf[0] = '\0';
		return 0;
	}
	const char *line = history[history_count - history_pos];
	unsigned int n = strlen(line);
	memcpy(buf, line, n);
	buf[n] = '\0';
	return (int)n;
}

static int read_line(char *buf)
{
	int len = 0;
	history_pos = 0;

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
		if (c == KEY_CTRL('c')) {
			sys_log("^C");
			len = 0;
			history_pos = 0;
			continue;
		}
		if (c == KEY_UP) {
			if (history_pos < history_count) {
				history_pos++;
				len = history_load(buf);
			}
			continue;
		}
		if (c == KEY_DOWN) {
			if (history_pos > 0) {
				history_pos--;
				len = history_load(buf);
			}
			continue;
		}
		if (c == KEY_LEFT || c == KEY_RIGHT)
			continue;               /* no cursor-position editing yet */
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
		history_push(line);

		char *rest = split(line);

		if (strcmp(line, "") == 0) {
			continue;
		} else if (strcmp(line, "help") == 0) {
			sys_log("shell: help ls cat head tail wc hexdump write append cp rm echo uptime sound history exit");
		} else if (strcmp(line, "history") == 0) {
			for (int i = 0; i < history_count; i++)
				sys_log(history[i]);
		} else if (strcmp(line, "echo") == 0) {
			sys_log(rest);
		} else if (strcmp(line, "uptime") == 0) {
			sys_printf("shell: %u ticks", sys_ticks());
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
		} else if (strcmp(line, "tail") == 0) {
			/* Last up to 64 bytes of the file, via the fd API (open/seek/
			 * read/close) instead of the one-shot named-file syscalls --
			 * exercises the VFS descriptor path the other commands don't. */
			int fd = sys_open(rest, 0);
			if (fd < 0) {
				sys_log("shell: no such file");
			} else {
				int size = sys_seek(fd, 0, SYS_SEEK_END);
				int start = size > 64 ? size - 64 : 0;
				sys_seek(fd, start, SYS_SEEK_SET);
				char data[65];
				int n = sys_read(fd, data, sizeof(data) - 1);
				sys_close(fd);
				if (n < 0) {
					sys_log("shell: read failed");
				} else {
					data[n] = '\0';
					sys_log(data);
				}
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
		} else if (strcmp(line, "append") == 0) {
			char *text = split(rest);
			if (sys_fappend(rest, text, strlen(text)) == 0)
				sys_log("shell: appended");
			else
				sys_log("shell: append failed");
		} else if (strcmp(line, "head") == 0) {
			/* First up to 64 bytes of the file, mirror image of tail. */
			int fd = sys_open(rest, 0);
			if (fd < 0) {
				sys_log("shell: no such file");
			} else {
				char data[65];
				int n = sys_read(fd, data, sizeof(data) - 1);
				sys_close(fd);
				if (n < 0) {
					sys_log("shell: read failed");
				} else {
					data[n] = '\0';
					sys_log(data);
				}
			}
		} else if (strcmp(line, "wc") == 0) {
			/* Byte count and newline count (lines = newlines, like wc -lc). */
			int fd = sys_open(rest, 0);
			if (fd < 0) {
				sys_log("shell: no such file");
			} else {
				int bytes = 0, lines = 0;
				char chunk[64];
				int n;
				while ((n = sys_read(fd, chunk, sizeof(chunk))) > 0) {
					bytes += n;
					for (int i = 0; i < n; i++)
						if (chunk[i] == '\n')
							lines++;
				}
				sys_close(fd);
				sys_printf("shell: %d lines  %d bytes  %s", lines, bytes, rest);
			}
		} else if (strcmp(line, "hexdump") == 0) {
			/* Classic hex + ASCII dump, 16 bytes per row. */
			int fd = sys_open(rest, 0);
			if (fd < 0) {
				sys_log("shell: no such file");
			} else {
				unsigned char row[16];
				int offset = 0;
				int n;
				while ((n = sys_read(fd, row, sizeof(row))) > 0) {
					/* Build "OOOO: HH HH .. HH  ASCII" into buf. */
					char buf[80];
					int p = 0;
					/* offset (4 hex digits) */
					char tmp[8];
					int tn = uitoa((unsigned int)offset, 16, tmp);
					for (int z = tn; z < 4; z++) buf[p++] = '0';
					for (int z = 0; z < tn; z++) buf[p++] = tmp[z];
					buf[p++] = ':'; buf[p++] = ' ';
					/* hex bytes */
					for (int i = 0; i < 16; i++) {
						if (i < n) {
							char h[4];
							int hn = uitoa(row[i], 16, h);
							if (hn < 2) buf[p++] = '0';
							for (int z = 0; z < hn; z++) buf[p++] = h[z];
						} else {
							buf[p++] = ' '; buf[p++] = ' ';
						}
						buf[p++] = ' ';
					}
					buf[p++] = ' ';
					/* ASCII */
					for (int i = 0; i < n; i++)
						buf[p++] = (row[i] >= 0x20 && row[i] < 0x7F) ? (char)row[i] : '.';
					buf[p] = '\0';
					sys_log(buf);
					offset += n;
				}
				sys_close(fd);
			}
		} else if (strcmp(line, "cp") == 0) {
			/* cp <src> <dst>: copy all bytes from one StinkFS file to another. */
			char *dst = split(rest);
			if (*rest == '\0' || *dst == '\0') {
				sys_log("shell: usage: cp <src> <dst>");
			} else {
				char data[128];
				int n = sys_fread(rest, data, sizeof(data) - 1);
				if (n < 0) {
					sys_log("shell: no such file");
				} else if (sys_fwrite(dst, data, (unsigned int)n) == 0) {
					sys_printf("shell: copied %d bytes to %s", n, dst);
				} else {
					sys_log("shell: write failed");
				}
			}
		} else if (strcmp(line, "sound") == 0) {
			int freq = atoi(rest);
			sys_tone((unsigned int)freq, 20);
			sys_printf("shell: played %d Hz", freq);
		} else {
			sys_printf("shell: unknown command: %s", line);
		}
	}
}
