/* StinkOS full-screen text editor: nano/DOS-edit style.
 * Open or create a file, edit with cursor movement, save with ^S or ^X. */
#include "libstink.h"

/* Screen geometry */
#define GLYPH    8
#define SCR_W    1024
#define SCR_H    768
#define COLS     (SCR_W / GLYPH)   /* 128 */
#define ROWS     (SCR_H / GLYPH)   /* 96  */

/* Buffer limits */
#define MAX_LINES 512
#define MAX_COL   127

/* Row assignments */
#define STATUS_ROW  0
#define EDIT_TOP    1
#define EDIT_BOT    94
#define EDIT_ROWS   (EDIT_BOT - EDIT_TOP + 1)   /* 94 */
#define HINT_ROW    95

/* Colors (same palette as the shell for consistency) */
#define C_BG     0x001022u
#define C_FG     0xC8D8FFu
#define C_BAR_BG 0x0A1E40u
#define C_BAR_FG 0x6090FFu
#define C_PR     0x00FF88u
#define C_CUR_BG 0x00FF88u
#define C_CUR_FG 0x001022u
#define C_ERR    0xFF5050u

/* Text buffer in BSS: 512 lines × 128 bytes = 64 KB (zero cost in ELF). */
static char lines[MAX_LINES][MAX_COL + 1];
static int  nlines;
static int  cur_r, cur_c;   /* cursor: row/col into lines[] */
static int  top_r;           /* first visible line index     */
static int  modified;
static char filename[MAX_COL + 1];

/* Shared I/O buffer — static so it doesn't live on the stack. */
static char iobuf[65536];

/* ---- helpers ---- */

static int line_len(int r) { return (int)strlen(lines[r]); }

static void clamp_col(void)
{
	int len = line_len(cur_r);
	if (cur_c > len)
		cur_c = len;
}

static void scroll_into_view(void)
{
	if (cur_r < top_r)
		top_r = cur_r;
	if (cur_r >= top_r + EDIT_ROWS)
		top_r = cur_r - EDIT_ROWS + 1;
}

/* ---- rendering ---- */

static void render_status(void)
{
	char buf[COLS + 1];
	snprintf(buf, sizeof(buf),
	         " EDIT: %-24s%s| Ln:%-4d/%d  Col:%d",
	         filename[0] ? filename : "(new file)",
	         modified ? "[*] " : "    ",
	         cur_r + 1, nlines, cur_c + 1);
	sys_fillrect(0, STATUS_ROW * GLYPH, SCR_W, GLYPH, C_BAR_BG);
	sys_drawtext(0, STATUS_ROW * GLYPH, buf, C_BAR_FG);
}

static void render_hints(void)
{
	sys_fillrect(0, HINT_ROW * GLYPH, SCR_W, GLYPH, C_BAR_BG);
	sys_drawtext(0, HINT_ROW * GLYPH,
	             " ^S Save  ^X Save+Quit  ^Q Quit  ^K Kill line  ^A Bol  ^E Eol",
	             C_BAR_FG);
}

static void render_line(int sr)
{
	int fr = top_r + sr;
	int y  = (EDIT_TOP + sr) * GLYPH;
	sys_fillrect(0, y, SCR_W, GLYPH, C_BG);
	if (fr < nlines && lines[fr][0] != '\0')
		sys_drawtext(0, y, lines[fr], C_FG);
}

static void render_cursor(void)
{
	int sr = cur_r - top_r;
	if (sr < 0 || sr >= EDIT_ROWS)
		return;
	int y = (EDIT_TOP + sr) * GLYPH;
	int x = cur_c * GLYPH;
	sys_fillrect(x, y, GLYPH, GLYPH, C_CUR_BG);
	char cb[2] = { lines[cur_r][cur_c] ? lines[cur_r][cur_c] : ' ', '\0' };
	sys_drawtext(x, y, cb, C_CUR_FG);
}

static void render_all(void)
{
	for (int sr = 0; sr < EDIT_ROWS; sr++)
		render_line(sr);
	render_status();
	render_hints();
	render_cursor();
}

/* Redraw only the current line, status and cursor (for single-char edits). */
static void render_partial(void)
{
	int sr = cur_r - top_r;
	if (sr >= 0 && sr < EDIT_ROWS)
		render_line(sr);
	render_cursor();
	render_status();
}

/* ---- buffer operations ---- */

static void buf_insert(int c, char ch)
{
	int len = line_len(cur_r);
	if (len >= MAX_COL)
		return;
	memmove(&lines[cur_r][c + 1], &lines[cur_r][c], (unsigned int)(len - c + 1));
	lines[cur_r][c] = ch;
	cur_c++;
	modified = 1;
}

static void buf_backspace(void)
{
	if (cur_c > 0) {
		int len = line_len(cur_r);
		memmove(&lines[cur_r][cur_c - 1],
		        &lines[cur_r][cur_c],
		        (unsigned int)(len - cur_c + 1));
		cur_c--;
		modified = 1;
	} else if (cur_r > 0) {
		int prev_len = line_len(cur_r - 1);
		int this_len = line_len(cur_r);
		if (prev_len + this_len <= MAX_COL) {
			memcpy(&lines[cur_r - 1][prev_len], lines[cur_r],
			       (unsigned int)(this_len + 1));
			for (int i = cur_r; i < nlines - 1; i++)
				memcpy(lines[i], lines[i + 1], MAX_COL + 1);
			memset(lines[nlines - 1], 0, MAX_COL + 1);
			nlines--;
			cur_r--;
			cur_c = prev_len;
			modified = 1;
			scroll_into_view();
		}
	}
}

static void buf_enter(void)
{
	if (nlines >= MAX_LINES)
		return;
	int len = line_len(cur_r);
	for (int i = nlines; i > cur_r + 1; i--)
		memcpy(lines[i], lines[i - 1], MAX_COL + 1);
	nlines++;
	memcpy(lines[cur_r + 1], &lines[cur_r][cur_c], (unsigned int)(len - cur_c + 1));
	lines[cur_r][cur_c] = '\0';
	cur_r++;
	cur_c = 0;
	modified = 1;
	scroll_into_view();
}

/* Kill to end of line; if already at end, join with next line. */
static void buf_kill(void)
{
	if (cur_c < line_len(cur_r)) {
		lines[cur_r][cur_c] = '\0';
		modified = 1;
	} else if (cur_r < nlines - 1) {
		for (int i = cur_r + 1; i < nlines - 1; i++)
			memcpy(lines[i], lines[i + 1], MAX_COL + 1);
		memset(lines[nlines - 1], 0, MAX_COL + 1);
		nlines--;
		modified = 1;
	}
}

/* ---- file I/O ---- */

static void load_file(void)
{
	int n = sys_fread(filename, iobuf, sizeof(iobuf) - 1);
	if (n <= 0) {
		nlines = 1;
		memset(lines[0], 0, MAX_COL + 1);
		return;
	}
	iobuf[n] = '\0';
	nlines = 0;
	int col = 0;
	for (int i = 0; i < n && nlines < MAX_LINES; i++) {
		char c = iobuf[i];
		if (c == '\n') {
			lines[nlines][col] = '\0';
			nlines++;
			col = 0;
		} else if (c != '\r' && col < MAX_COL) {
			lines[nlines][col++] = c;
		}
	}
	/* Flush last line (may lack trailing newline). */
	lines[nlines][col] = '\0';
	nlines++;
	if (nlines == 0) {
		nlines = 1;
		memset(lines[0], 0, MAX_COL + 1);
	}
}

static void save_file(void)
{
	int pos = 0;
	for (int r = 0; r < nlines; r++) {
		int len = line_len(r);
		if (pos + len + 2 > (int)sizeof(iobuf))
			break;
		memcpy(&iobuf[pos], lines[r], (unsigned int)len);
		pos += len;
		if (r < nlines - 1)
			iobuf[pos++] = '\n';
	}
	sys_fwrite(filename, iobuf, (unsigned int)pos);
	modified = 0;
}

/* ---- filename prompt ---- */

static void prompt_filename(void)
{
	char prompt[COLS + 1];
	int  plen = 0;
	filename[0] = '\0';

	sys_fillrect(0, 0, SCR_W, SCR_H, C_BG);
	sys_drawtext(0, 0,
	             "  StinkOS EDIT  --  filename to open (empty = new file, Enter to confirm):",
	             C_BAR_FG);

	for (;;) {
		snprintf(prompt, sizeof(prompt), " > %.*s_", COLS - 5, filename);
		sys_fillrect(0, GLYPH * 2, SCR_W, GLYPH, C_BG);
		sys_drawtext(0, GLYPH * 2, prompt, C_PR);

		int c = sys_getkey();
		if (c == 0)
			continue;
		if (c == '\n' || c == '\r')
			break;
		if (c == '\b' && plen > 0)
			filename[--plen] = '\0';
		else if (c >= 32 && c < 127 && plen < MAX_COL)
			filename[plen++] = (char)c;
		filename[plen] = '\0';
	}
}

/* ---- main ---- */

void main(void)
{
	prompt_filename();
	load_file();

	cur_r = 0; cur_c = 0; top_r = 0; modified = 0;
	render_all();

	for (;;) {
		int c = sys_getkey();
		if (c == 0)
			continue;

		/* Navigation — full repaint (viewport may have shifted). */
		if (c == KEY_UP) {
			if (cur_r > 0) { cur_r--; clamp_col(); }
			scroll_into_view(); render_all(); continue;
		}
		if (c == KEY_DOWN) {
			if (cur_r < nlines - 1) { cur_r++; clamp_col(); }
			scroll_into_view(); render_all(); continue;
		}
		if (c == KEY_LEFT) {
			if (cur_c > 0) {
				cur_c--; render_partial();
			} else if (cur_r > 0) {
				cur_r--; cur_c = line_len(cur_r);
				scroll_into_view(); render_all();
			}
			continue;
		}
		if (c == KEY_RIGHT) {
			int len = line_len(cur_r);
			if (cur_c < len) {
				cur_c++; render_partial();
			} else if (cur_r < nlines - 1) {
				cur_r++; cur_c = 0;
				scroll_into_view(); render_all();
			}
			continue;
		}
		if (c == KEY_HOME || c == KEY_CTRL('a')) {
			cur_c = 0; render_partial(); continue;
		}
		if (c == KEY_END || c == KEY_CTRL('e')) {
			cur_c = line_len(cur_r); render_partial(); continue;
		}
		if (c == KEY_PGUP) {
			cur_r -= EDIT_ROWS;
			if (cur_r < 0) cur_r = 0;
			clamp_col(); scroll_into_view(); render_all(); continue;
		}
		if (c == KEY_PGDN) {
			cur_r += EDIT_ROWS;
			if (cur_r >= nlines) cur_r = nlines - 1;
			clamp_col(); scroll_into_view(); render_all(); continue;
		}

		/* Control commands */
		if (c == KEY_CTRL('s')) { save_file(); render_status(); continue; }
		if (c == KEY_CTRL('x')) { save_file(); sys_exit(); return; }
		if (c == KEY_CTRL('q')) { sys_exit(); return; }
		if (c == KEY_CTRL('k')) { buf_kill(); render_all(); continue; }

		/* Editing */
		if (c == '\b') {
			buf_backspace();
			scroll_into_view(); render_all(); continue;
		}
		if (c == '\n' || c == '\r') {
			buf_enter();
			render_all(); continue;
		}
		if (c >= 32 && c < 127) {
			buf_insert(cur_c, (char)c);
			render_partial(); continue;
		}
	}
}
