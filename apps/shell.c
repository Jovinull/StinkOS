/* StinkOS graphical terminal shell. All input and output is rendered on screen
 * via sys_drawtext / sys_fillrect. The prompt echoes keystrokes in real time;
 * backspace and command history (arrow keys) update the line in place. */
#include "libstink.h"

/* ---- terminal layer ---- */
#define TERM_W     1024
#define TERM_H     768
#define TERM_GLYPH 8
#define TERM_COLS  (TERM_W / TERM_GLYPH)   /* 128 columns */
#define TERM_ROWS  (TERM_H / TERM_GLYPH)   /* 96 rows     */
#define TERM_BG    0x001022u
#define TERM_FG    0xC8D8FFu   /* normal output: light blue-white */
#define TERM_IN    0xFFFFFFu   /* committed input line: white     */
#define TERM_PR    0x00FF88u   /* prompt + live input: green      */
#define TERM_ERR   0xFF5050u   /* error messages: light red       */
#define TERM_HEAD  0x6090FFu   /* header / info lines: medium blue */

/* Scrollback ring buffer. The terminal keeps the last LOG_MAX output lines
 * (overwriting oldest on overflow) plus a scroll_offset that PgUp/PgDn move
 * up and down. scroll_offset = 0 means "pinned to the bottom"; positive
 * values scroll the view up by that many lines. New output forces the view
 * back to the bottom so the user never misses a fresh line. */
#define LOG_MAX  256
#define SCROLL_PAGE  (TERM_ROWS - 4)

static int term_row = 0;
static char         log_text [LOG_MAX][TERM_COLS + 1];
static unsigned int log_color[LOG_MAX];
static int          log_count;          /* number of populated slots (cap LOG_MAX) */
static int          log_head;           /* next slot to write */
static int          scroll_offset;      /* lines scrolled up from bottom */

static void term_clear(void)
{
	sys_fillrect(0, 0, TERM_W, TERM_H, TERM_BG);
	term_row = 0;
}

static void term_erase(int row)
{
	sys_fillrect(0, row * TERM_GLYPH, TERM_W, TERM_GLYPH, TERM_BG);
}

/* Append one line to the scrollback ring. */
static void log_push(const char *s, unsigned int col)
{
	int slot = log_head;
	unsigned int i = 0;
	while (s[i] && i < TERM_COLS) {
		log_text[slot][i] = s[i];
		i++;
	}
	log_text[slot][i] = '\0';
	log_color[slot] = col;
	log_head = (log_head + 1) % LOG_MAX;
	if (log_count < LOG_MAX) log_count++;
}

/* Redraw the visible window of the scrollback ring at the current
 * scroll_offset. Reserves the bottom row for the live input prompt that
 * read_line manages. */
static void redraw_scrollback(void)
{
	sys_fillrect(0, 0, TERM_W, TERM_H, TERM_BG);
	int visible = TERM_ROWS - 1;
	int total   = log_count;
	int start   = total - visible - scroll_offset;
	if (start < 0) start = 0;
	int end = start + visible;
	if (end > total) end = total;

	int row = 0;
	for (int i = start; i < end; i++) {
		int slot = (log_head - log_count + i + LOG_MAX) % LOG_MAX;
		sys_drawtext(0, row * TERM_GLYPH, log_text[slot], log_color[slot]);
		row++;
	}
	term_row = row;
}

static void term_print(const char *s, unsigned int col)
{
	log_push(s, col);
	scroll_offset = 0;                    /* new output jumps back to bottom */
	if (term_row >= TERM_ROWS - 1) {
		redraw_scrollback();          /* scroll the window up by one line */
	} else {
		term_erase(term_row);
		sys_drawtext(0, term_row * TERM_GLYPH, s, col);
		term_row++;
	}
}

static void tprintf(const char *fmt, ...)
{
	char buf[TERM_COLS + 1];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	term_print(buf, TERM_FG);
}

/* ---- command history ---- */
#define LINE_MAX     64
#define HISTORY_MAX  8

static char history[HISTORY_MAX][LINE_MAX];
static int  history_count;
static int  history_pos;

#define HISTORY_FILE "SHELL.HIS"

/* Serialize the in-memory history array into one line per entry and write it
 * back to SHELL.HIS so the next boot can replay it. Keeps the file size to
 * at most HISTORY_MAX * LINE_MAX bytes (~512 bytes), well within StinkFS. */
static void history_save(void)
{
	char buf[HISTORY_MAX * LINE_MAX];
	int  off = 0;
	for (int i = 0; i < history_count; i++) {
		for (int j = 0; history[i][j] && off < (int)sizeof(buf) - 1; j++)
			buf[off++] = history[i][j];
		if (off < (int)sizeof(buf))
			buf[off++] = '\n';
	}
	sys_fwrite(HISTORY_FILE, buf, (unsigned int)off);
}

/* Populate the in-memory history array from SHELL.HIS at startup. Silently
 * starts with an empty history if the file is missing or unreadable; the
 * first command written triggers a fresh history_save(). */
static void history_load_persistent(void)
{
	char buf[HISTORY_MAX * LINE_MAX];
	int  n = sys_fread(HISTORY_FILE, buf, sizeof(buf));
	if (n <= 0)
		return;
	int ls = 0;
	for (int i = 0; i < n; i++) {
		if (buf[i] != '\n')
			continue;
		int len = i - ls;
		if (len > LINE_MAX - 1) len = LINE_MAX - 1;
		if (len > 0 && history_count < HISTORY_MAX) {
			for (int k = 0; k < len; k++)
				history[history_count][k] = buf[ls + k];
			history[history_count][len] = '\0';
			history_count++;
		}
		ls = i + 1;
	}
}

static void history_push(const char *line)
{
	if (line[0] == '\0')
		return;
	unsigned int n = strlen(line);
	if (n > LINE_MAX - 1) n = LINE_MAX - 1;
	if (history_count == HISTORY_MAX) {
		for (int i = 1; i < HISTORY_MAX; i++)
			memcpy(history[i - 1], history[i], LINE_MAX);
		history_count--;
	}
	memcpy(history[history_count], line, n);
	history[history_count][n] = '\0';
	history_count++;
	history_save();
}

static int history_load(char *buf)
{
	if (history_pos == 0) { buf[0] = '\0'; return 0; }
	const char *line = history[history_count - history_pos];
	unsigned int n = strlen(line);
	memcpy(buf, line, n);
	buf[n] = '\0';
	return (int)n;
}

/* ---- system config (STINK.CONF) ---- */

#define HOSTNAME_MAX  32
#define CONFIG_FILE   "STINK.CONF"

static char hostname[HOSTNAME_MAX] = "stinkos";

/* Parse one "key=value" line out of STINK.CONF: writes the value at the named
 * key into `out_buf` (cap-bounded, NUL-terminated). Silently leaves `out_buf`
 * untouched if the key is absent or the file is missing. Unknown keys are
 * ignored so STINK.CONF stays forward-compatible. */
static void config_get(const char *key, char *out_buf, unsigned int cap)
{
	char raw[512];
	int  n = sys_fread(CONFIG_FILE, raw, sizeof(raw) - 1);
	if (n <= 0)
		return;
	raw[n] = '\0';

	unsigned int klen = 0;
	while (key[klen]) klen++;

	int ls = 0;
	for (int i = 0; i <= n; i++) {
		if (raw[i] != '\n' && raw[i] != '\0')
			continue;
		raw[i] = '\0';
		const char *line = raw + ls;
		int match = 1;
		for (unsigned int j = 0; j < klen; j++) {
			if (line[j] != key[j]) { match = 0; break; }
		}
		if (match && line[klen] == '=') {
			const char *p = line + klen + 1;
			unsigned int w = 0;
			while (*p && w + 1 < cap)
				out_buf[w++] = *p++;
			out_buf[w] = '\0';
			return;
		}
		ls = i + 1;
	}
}

/* Rewrite STINK.CONF with a single key=value line. Other keys are dropped
 * (no need to preserve them while hostname is the only documented field). */
static void config_set_hostname(const char *new_name)
{
	char line[HOSTNAME_MAX + 16];
	int p = 0;
	const char *prefix = "hostname=";
	for (int i = 0; prefix[i]; i++) line[p++] = prefix[i];
	for (int i = 0; new_name[i] && p < (int)sizeof(line) - 1; i++)
		line[p++] = new_name[i];
	line[p++] = '\n';
	sys_fwrite(CONFIG_FILE, line, (unsigned int)p);

	int j = 0;
	for (; new_name[j] && j < HOSTNAME_MAX - 1; j++)
		hostname[j] = new_name[j];
	hostname[j] = '\0';
}

/* ---- input line rendering ---- */
static void render_input(const char *buf, int len, int with_cursor)
{
	char display[TERM_COLS + 1];
	int p = 0;
	for (int i = 0; hostname[i] && p < TERM_COLS - 4; i++)
		display[p++] = hostname[i];
	display[p++] = '>';
	display[p++] = ' ';
	for (int i = 0; i < len && p < TERM_COLS - 1; i++)
		display[p++] = buf[i];
	if (with_cursor && p < TERM_COLS)
		display[p++] = '_';
	display[p] = '\0';
	term_erase(term_row);
	sys_drawtext(0, term_row * TERM_GLYPH, display, with_cursor ? TERM_PR : TERM_IN);
}

static int read_line(char *buf)
{
	int len = 0;
	history_pos = 0;
	if (term_row >= TERM_ROWS)
		term_clear();
	render_input(buf, len, 1);

	for (;;) {
		int c = sys_getkey();
		if (c == 0) continue;

		if (c == '\n') {
			buf[len] = '\0';
			render_input(buf, len, 0);   /* freeze without cursor */
			term_row++;
			if (term_row >= TERM_ROWS) term_clear();
			return len;
		}
		if (c == '\b') {
			if (len > 0) { len--; render_input(buf, len, 1); }
			continue;
		}
		if (c == KEY_CTRL('c')) {
			len = 0; buf[0] = '\0'; history_pos = 0;
			/* Show ^C on the current line then open a fresh prompt. */
			char ctrlc[5] = {'>',  ' ', '^', 'C', '\0'};
			term_erase(term_row);
			sys_drawtext(0, term_row * TERM_GLYPH, ctrlc, TERM_ERR);
			term_row++;
			if (term_row >= TERM_ROWS) term_clear();
			render_input(buf, 0, 1);
			continue;
		}
		if (c == KEY_UP) {
			if (history_pos < history_count) {
				history_pos++;
				len = history_load(buf);
				render_input(buf, len, 1);
			}
			continue;
		}
		if (c == KEY_DOWN) {
			if (history_pos > 0) {
				history_pos--;
				len = history_load(buf);
				render_input(buf, len, 1);
			}
			continue;
		}
		if (c == KEY_LEFT || c == KEY_RIGHT)
			continue;
		if (c == KEY_PGUP) {
			int max = log_count - (TERM_ROWS - 1);
			if (max < 0) max = 0;
			scroll_offset += SCROLL_PAGE;
			if (scroll_offset > max) scroll_offset = max;
			redraw_scrollback();
			render_input(buf, len, 1);
			continue;
		}
		if (c == KEY_PGDN) {
			scroll_offset -= SCROLL_PAGE;
			if (scroll_offset < 0) scroll_offset = 0;
			redraw_scrollback();
			render_input(buf, len, 1);
			continue;
		}
		if (c >= 32 && c < 127 && len < LINE_MAX - 1) {
			buf[len++] = (char)c;
			render_input(buf, len, 1);
		}
	}
}

/* ---- helpers ---- */
static char *split(char *line)
{
	for (char *p = line; *p != '\0'; p++) {
		if (*p == ' ') { *p = '\0'; return p + 1; }
	}
	return line + strlen(line);
}

/* Write a padded filename + size line: "name             1234 B". */
static void print_fileinfo(const char *name, int size)
{
	char line[TERM_COLS + 1];
	int p = 0;
	for (int j = 0; name[j] != '\0' && p < 16; j++) line[p++] = name[j];
	while (p < 17) line[p++] = ' ';
	char num[12]; int n = uitoa((unsigned int)size, 10, num);
	for (int i = 0; i < n; i++) line[p++] = num[i];
	line[p++] = ' '; line[p++] = 'B'; line[p] = '\0';
	term_print(line, TERM_FG);
}

/* ---- main ---- */
void main(void)
{
	term_clear();
	config_get("hostname", hostname, sizeof(hostname));
	history_load_persistent();
	term_print("STINKOS SHELL  (type 'help', 'exit' to quit)", TERM_HEAD);
	term_row++;   /* blank line after header */

	for (;;) {
		char line[LINE_MAX];
		read_line(line);
		history_push(line);

		char *rest = split(line);

		if (line[0] == '\0') {
			continue;
		} else if (strcmp(line, "help") == 0) {
			term_print("ls  cat  head  tail  wc  hexdump  write  append", TERM_FG);
			term_print("cp  mv  rm  touch  grep  echo  uptime  sound", TERM_FG);
			term_print("hostname [name]  run <appname>  netinfo  netstat  ping <ip>  mem  dmesg  ps  history  exit", TERM_FG);
			term_print("kill <pid>  suspend <pid>  resume <pid>", TERM_FG);
		} else if (strcmp(line, "history") == 0) {
			for (int i = 0; i < history_count; i++)
				term_print(history[i], TERM_FG);
		} else if (strcmp(line, "echo") == 0) {
			term_print(rest, TERM_FG);
		} else if (strcmp(line, "uptime") == 0) {
			tprintf("%u ticks since boot", sys_ticks());
		} else if (strcmp(line, "netstat") == 0) {
			static const char *state_name[] = {
				"CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD",
				"ESTAB", "FIN_W1", "FIN_W2", "CLOSE_W",
				"CLOSING", "LAST_ACK", "TIME_W"
			};
			term_print("idx state    local         remote        rx    tx", TERM_HEAD);
			int active = 0;
			for (int i = 0; i < SYS_TCP_SLOT_MAX; i++) {
				struct sys_tcp_info t;
				if (sys_netstat(i, &t) != 0 || !t.in_use) continue;
				active++;
				const char *st = (t.state < sizeof(state_name) / sizeof(state_name[0]))
				                 ? state_name[t.state] : "?";
				/* Render remote IP as a.b.c.d (network byte order: lowest byte first). */
				unsigned char r0 = (unsigned char)(t.remote_ip      );
				unsigned char r1 = (unsigned char)(t.remote_ip >>  8);
				unsigned char r2 = (unsigned char)(t.remote_ip >> 16);
				unsigned char r3 = (unsigned char)(t.remote_ip >> 24);
				tprintf("%d   %-8s :%-5u  %u.%u.%u.%u:%-5u  %u   %u",
				        i, st, (unsigned)t.local_port,
				        r0, r1, r2, r3, (unsigned)t.remote_port,
				        t.rx_pending, t.tx_pending);
			}
			if (!active) term_print("no active TCBs", TERM_FG);
		} else if (strcmp(line, "kill") == 0) {
			if (rest[0] == '\0') {
				term_print("usage: kill <pid>", TERM_ERR);
			} else {
				int pid = 0;
				for (int j = 0; rest[j] >= '0' && rest[j] <= '9'; j++)
					pid = pid * 10 + (rest[j] - '0');
				int rc = sys_kill(pid);
				if (rc == 0) tprintf("killed pid %d", pid);
				else         tprintf("kill pid %d failed", pid);
			}
		} else if (strcmp(line, "suspend") == 0) {
			if (rest[0] == '\0') {
				term_print("usage: suspend <pid>", TERM_ERR);
			} else {
				int pid = 0;
				for (int j = 0; rest[j] >= '0' && rest[j] <= '9'; j++)
					pid = pid * 10 + (rest[j] - '0');
				if (sys_suspend(pid) == 0) tprintf("suspended pid %d", pid);
				else                        tprintf("suspend pid %d failed", pid);
			}
		} else if (strcmp(line, "resume") == 0) {
			if (rest[0] == '\0') {
				term_print("usage: resume <pid>", TERM_ERR);
			} else {
				int pid = 0;
				for (int j = 0; rest[j] >= '0' && rest[j] <= '9'; j++)
					pid = pid * 10 + (rest[j] - '0');
				if (sys_resume(pid) == 0) tprintf("resumed pid %d", pid);
				else                       tprintf("resume pid %d failed", pid);
			}
		} else if (strcmp(line, "ps") == 0) {
			static char ps_buf[1024];
			int n = sys_proc_info(ps_buf, sizeof(ps_buf));
			if (n <= 0) {
				term_print("(no procs)", TERM_FG);
			} else {
				int ls = 0;
				for (int i = 0; i < n; i++) {
					if (ps_buf[i] == '\n' || i == n - 1) {
						int end = (i == n - 1 && ps_buf[i] != '\n') ? i + 1 : i;
						char l2[TERM_COLS + 1];
						int copy = end - ls;
						if (copy > TERM_COLS) copy = TERM_COLS;
						for (int j = 0; j < copy; j++) l2[j] = ps_buf[ls + j];
						l2[copy] = '\0';
						if (copy > 0) term_print(l2, TERM_FG);
						ls = i + 1;
					}
				}
			}
		} else if (strcmp(line, "dmesg") == 0) {
			static char klog_buf[4096];
			int n = sys_klog_read(klog_buf, sizeof(klog_buf));
			if (n <= 0) {
				term_print("(klog empty)", TERM_FG);
			} else {
				int ls = 0;
				for (int i = 0; i < n; i++) {
					if (klog_buf[i] == '\n' || i == n - 1) {
						int end = (i == n - 1 && klog_buf[i] != '\n') ? i + 1 : i;
						char line2[TERM_COLS + 1];
						int copy = end - ls;
						if (copy > TERM_COLS) copy = TERM_COLS;
						for (int j = 0; j < copy; j++) line2[j] = klog_buf[ls + j];
						line2[copy] = '\0';
						if (copy > 0) term_print(line2, TERM_FG);
						ls = i + 1;
					}
				}
			}
		} else if (strcmp(line, "mem") == 0) {
			struct sys_meminfo m;
			if (sys_meminfo(&m) != 0) {
				term_print("meminfo failed", TERM_ERR);
			} else {
				unsigned int used = m.total_pages - m.free_pages;
				tprintf("phys: %u/%u pages (%u KiB used / %u KiB total)",
				        used, m.total_pages,
				        used * 4, m.total_pages * 4);
				tprintf("user brk: 0x%x", m.user_brk);
			}
		} else if (strcmp(line, "hostname") == 0) {
			if (rest[0] == '\0') {
				term_print(hostname, TERM_FG);
			} else {
				config_set_hostname(rest);
				tprintf("hostname set to %s (persisted)", hostname);
			}
		} else if (strcmp(line, "exit") == 0) {
			term_print("bye", TERM_HEAD);
			sys_exit();
		} else if (strcmp(line, "ls") == 0) {
			int n = sys_fcount();
			char name[16];
			for (int i = 0; i < n; i++) {
				int sz = sys_finfo(i, name);
				if (sz >= 0) { name[15] = '\0'; print_fileinfo(name, sz); }
			}
			tprintf("%d file(s)", n);
		} else if (strcmp(line, "cat") == 0) {
			char data[LINE_MAX];
			int n = sys_fread(rest, data, sizeof(data) - 1);
			if (n < 0) term_print("no such file", TERM_ERR);
			else { data[n] = '\0'; term_print(data, TERM_FG); }
		} else if (strcmp(line, "tail") == 0) {
			int fd = sys_open(rest, 0);
			if (fd < 0) { term_print("no such file", TERM_ERR); }
			else {
				int size  = sys_seek(fd, 0, SYS_SEEK_END);
				int start = size > 64 ? size - 64 : 0;
				sys_seek(fd, start, SYS_SEEK_SET);
				char data[65];
				int n = sys_read(fd, data, sizeof(data) - 1);
				sys_close(fd);
				if (n < 0) term_print("read failed", TERM_ERR);
				else { data[n] = '\0'; term_print(data, TERM_FG); }
			}
		} else if (strcmp(line, "head") == 0) {
			int fd = sys_open(rest, 0);
			if (fd < 0) { term_print("no such file", TERM_ERR); }
			else {
				char data[65];
				int n = sys_read(fd, data, sizeof(data) - 1);
				sys_close(fd);
				if (n < 0) term_print("read failed", TERM_ERR);
				else { data[n] = '\0'; term_print(data, TERM_FG); }
			}
		} else if (strcmp(line, "wc") == 0) {
			int fd = sys_open(rest, 0);
			if (fd < 0) { term_print("no such file", TERM_ERR); }
			else {
				int bytes = 0, lines = 0;
				char chunk[64]; int n;
				while ((n = sys_read(fd, chunk, sizeof(chunk))) > 0) {
					bytes += n;
					for (int i = 0; i < n; i++) if (chunk[i] == '\n') lines++;
				}
				sys_close(fd);
				tprintf("%d lines  %d bytes  %s", lines, bytes, rest);
			}
		} else if (strcmp(line, "hexdump") == 0) {
			int fd = sys_open(rest, 0);
			if (fd < 0) { term_print("no such file", TERM_ERR); }
			else {
				unsigned char row[16]; int offset = 0, n;
				while ((n = sys_read(fd, row, sizeof(row))) > 0) {
					char buf[80]; int p = 0;
					char tmp[8]; int tn = uitoa((unsigned int)offset, 16, tmp);
					for (int z = tn; z < 4; z++) buf[p++] = '0';
					for (int z = 0; z < tn; z++) buf[p++] = tmp[z];
					buf[p++] = ':'; buf[p++] = ' ';
					for (int i = 0; i < 16; i++) {
						if (i < n) {
							char h[4]; int hn = uitoa(row[i], 16, h);
							if (hn < 2) buf[p++] = '0';
							for (int z = 0; z < hn; z++) buf[p++] = h[z];
						} else { buf[p++] = ' '; buf[p++] = ' '; }
						buf[p++] = ' ';
					}
					buf[p++] = ' ';
					for (int i = 0; i < n; i++)
						buf[p++] = (row[i] >= 0x20 && row[i] < 0x7F) ? (char)row[i] : '.';
					buf[p] = '\0';
					term_print(buf, TERM_FG);
					offset += n;
				}
				sys_close(fd);
			}
		} else if (strcmp(line, "write") == 0) {
			char *text = split(rest);
			if (sys_fwrite(rest, text, strlen(text)) == 0) term_print("written", TERM_FG);
			else term_print("write failed", TERM_ERR);
		} else if (strcmp(line, "append") == 0) {
			char *text = split(rest);
			if (sys_fappend(rest, text, strlen(text)) == 0) term_print("appended", TERM_FG);
			else term_print("append failed", TERM_ERR);
		} else if (strcmp(line, "rm") == 0) {
			if (sys_fdelete(rest) == 0) term_print("removed", TERM_FG);
			else term_print("no such file", TERM_ERR);
		} else if (strcmp(line, "cp") == 0) {
			char *dst = split(rest);
			if (*rest == '\0' || *dst == '\0') {
				term_print("usage: cp <src> <dst>", TERM_ERR);
			} else {
				char data[128];
				int n = sys_fread(rest, data, sizeof(data) - 1);
				if (n < 0) term_print("no such file", TERM_ERR);
				else if (sys_fwrite(dst, data, (unsigned int)n) == 0)
					tprintf("copied %d bytes -> %s", n, dst);
				else term_print("write failed", TERM_ERR);
			}
		} else if (strcmp(line, "mv") == 0) {
			char *dst = split(rest);
			if (*rest == '\0' || *dst == '\0') {
				term_print("usage: mv <src> <dst>", TERM_ERR);
			} else {
				char data[128];
				int n = sys_fread(rest, data, sizeof(data) - 1);
				if (n < 0) term_print("no such file", TERM_ERR);
				else if (sys_fwrite(dst, data, (unsigned int)n) != 0)
					term_print("write failed", TERM_ERR);
				else { sys_fdelete(rest); tprintf("moved -> %s", dst); }
			}
		} else if (strcmp(line, "touch") == 0) {
			int fd = sys_open(rest, 0);
			if (fd >= 0) { sys_close(fd); }
			else {
				char nul = '\0';
				if (sys_fwrite(rest, &nul, 1) == 0) tprintf("created %s", rest);
				else term_print("touch failed", TERM_ERR);
			}
		} else if (strcmp(line, "grep") == 0) {
			char *fname = split(rest);
			if (*rest == '\0' || *fname == '\0') {
				term_print("usage: grep <pattern> <file>", TERM_ERR);
			} else {
				int fd = sys_open(fname, 0);
				if (fd < 0) { term_print("no such file", TERM_ERR); }
				else {
					char lbuf[64]; int lp = 0;
					char rbuf[64]; int rn;
					while ((rn = sys_read(fd, rbuf, sizeof(rbuf))) > 0) {
						for (int i = 0; i < rn; i++) {
							char c = rbuf[i];
							if (c == '\n' || lp == (int)sizeof(lbuf) - 1) {
								lbuf[lp] = '\0';
								if (strstr(lbuf, rest)) term_print(lbuf, TERM_FG);
								lp = 0;
							} else { lbuf[lp++] = c; }
						}
					}
					sys_close(fd);
				}
			}
		} else if (strcmp(line, "sound") == 0) {
			int freq = atoi(rest);
			sys_tone((unsigned int)freq, 20);
			tprintf("played %d Hz", freq);
		} else if (strcmp(line, "run") == 0) {
			if (*rest == '\0') {
				term_print("usage: run <appname>", TERM_ERR);
			} else {
				tprintf("launching %s...", rest);
				if (sys_exec(rest) < 0)
					tprintf("not found: %s", rest);
			}
		} else if (strcmp(line, "netinfo") == 0 || strcmp(line, "ifconfig") == 0) {
			struct sys_net_info ni;
			if (sys_netinfo(&ni) != 0) {
				term_print("netinfo unavailable", TERM_ERR);
			} else {
				static const char *st[] = { "init", "discovering",
				                            "requesting", "bound", "failed" };
				unsigned char *ip = (unsigned char *)&ni.ip;
				unsigned char *mk = (unsigned char *)&ni.mask;
				unsigned char *gw = (unsigned char *)&ni.gateway;
				unsigned char *dn = (unsigned char *)&ni.dns;
				term_print(ni.link_up ? "link: up" : "link: down",
				           ni.link_up ? TERM_FG : TERM_ERR);
				tprintf("mac:  %02x:%02x:%02x:%02x:%02x:%02x",
				        ni.mac[0], ni.mac[1], ni.mac[2],
				        ni.mac[3], ni.mac[4], ni.mac[5]);
				tprintf("ip:   %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
				tprintf("mask: %u.%u.%u.%u", mk[0], mk[1], mk[2], mk[3]);
				tprintf("gw:   %u.%u.%u.%u", gw[0], gw[1], gw[2], gw[3]);
				tprintf("dns:  %u.%u.%u.%u", dn[0], dn[1], dn[2], dn[3]);
				tprintf("dhcp: %s", ni.dhcp_state <= 4 ? st[ni.dhcp_state] : "?");
			}
		} else if (strcmp(line, "ping") == 0) {
			unsigned int oct[4]; int seg = 0;
			unsigned int cur = 0; int digits = 0, bad = 0;
			for (const char *p = rest; ; p++) {
				char ch = *p;
				if (ch >= '0' && ch <= '9') {
					cur = cur * 10 + (unsigned int)(ch - '0');
					digits++;
					if (cur > 255) { bad = 1; break; }
				} else if (ch == '.' || ch == '\0') {
					if (!digits || seg >= 4) { bad = 1; break; }
					oct[seg++] = cur; cur = 0; digits = 0;
					if (ch == '\0') break;
				} else { bad = 1; break; }
			}
			if (bad || seg != 4) {
				term_print("usage: ping <a.b.c.d>", TERM_ERR);
			} else {
				unsigned int ip = sys_ip4(oct[0], oct[1], oct[2], oct[3]);
				int rtt = sys_ping(ip, 1000);
				if (rtt < 0) tprintf("%s: no reply", rest);
				else         tprintf("reply from %s: %d ms", rest, rtt);
			}
		} else {
			tprintf("unknown command: %s", line);
		}
	}
}
