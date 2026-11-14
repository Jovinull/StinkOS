/* win.c -- kernel window manager and compositor.
 *
 * Model
 * -----
 * Everything that draws window chrome, composes the screen or routes input
 * runs in one kernel thread, `kwm`. Interrupt handlers only record raw
 * input (keyboard.c, mouse.c) and wake the thread. The kernel does not
 * preempt itself, so the thread and the window syscalls exclude each
 * other without locks: a syscall sees the window table between two
 * complete compositor steps, never in the middle of one.
 *
 * Screen updates are damage-driven. The thread keeps a full-screen back
 * buffer holding the composed scene (desktop, windows, chrome, taskbar):
 *   scene damage   -- part of the scene changed; recompose that rectangle
 *                     of the back buffer from the window buffers.
 *   present damage -- part of the screen no longer matches the back
 *                     buffer (always includes scene damage; the mouse
 *                     cursor adds its old position). Copy it to video
 *                     memory, then draw the cursor on top.
 * Nothing is drawn to video memory except through that copy, so a window
 * that moves, closes or is uncovered leaves no stale pixels behind, and
 * overlapping windows never flicker through each other.
 *
 * Window pixels belong to the window: frames allocated here, mapped into
 * the owner at USER_WIN_BASE with PG_SHARED so tearing the address space
 * down does not free them a second time.
 *
 * Input: the pointer goes to the window under it (or to the one that got
 * the button press, until release); keys go to the focused window's own
 * queues. Programs without a window that draw straight to the framebuffer
 * take the whole screen while they run (win_legacy_draw).
 */
#include "win.h"
#include "defs.h"
#include "memlayout.h"
#include "cpuid.h"

/* ---- palette ------------------------------------------------------------- */

#define C_DESKTOP      0x0d1117u
#define C_BORDER       0x30363du
#define C_BORDER_FOCUS 0x3d6b4fu
#define C_TITLE_BG     0x161b22u
#define C_TITLE_FOCUS  0x1f2933u
#define C_TITLE_FG     0x8b949eu
#define C_TITLE_FG_F   0xe6edf3u
#define C_CLOSE_HOVER  0xda3633u
#define C_BAR_BG       0x0b0f14u
#define C_BAR_LINE     0x30363du
#define C_BTN_BG       0x161b22u
#define C_BTN_FOCUS    0x263241u
#define C_ACCENT       0x57f287u
#define C_BAND         0x57f287u

#define TASK_BTN_W     136
#define TASK_BTN_GAP   4
#define TASK_BTN_X0    84          /* after the "StinkOS" label */
#define CLOSE_W        18
#define GRIP           14

/* ---- state --------------------------------------------------------------- */

#define EVQ   64
#define KEYQ  32

struct win {
	int           pid;                 /* owner, 0 = free slot */
	int           x, y;                /* client area, screen coords */
	int           w, h;                /* client size in pixels */
	unsigned int  flags;
	int           visible;
	unsigned int  z;                   /* higher = closer to the viewer */
	char          title[40];
	unsigned int  frames[WIN_MAX_FRAMES];
	int           n_frames;

	struct win_event evq[EVQ];
	int           ev_r, ev_w;
	char          keyq[KEYQ];          /* SYS_GETKEY view of the key stream */
	int           key_r, key_w;
	unsigned int  scanq[KEYQ];         /* SYS_GETKEYEVENT view */
	int           scan_r, scan_w;
	struct proc  *waiter;              /* blocked in win_wait_event */
};

struct rect { int x0, y0, x1, y1; };  /* x1, y1 exclusive */

static struct win    wins[WIN_MAX];
static unsigned int *back;             /* composed scene, scr_w * scr_h XRGB */
static int           scr_w, scr_h;
static struct proc  *kwm;

static int           focus = -1;       /* index into wins[] */
static unsigned int  z_top = 1;
static int           fs_pid;           /* legacy program holding the whole screen */

static struct rect   scene;            /* to recompose */
static struct rect   present;          /* to copy to the screen */

static int           grab = -1;        /* window that owns the pointer while a button is down */
static int           drag = -1, drag_dx, drag_dy;
static int           rsz = -1, rsz_w, rsz_h;
static int           hover_close = -1;
static int           last_mx = -1, last_my = -1;
static unsigned char last_mb;
static int           cur_drawn, cur_x, cur_y;

static char          clock_txt[6] = "--:--";
static unsigned int  clock_tick;
static unsigned int  frame_tick;

/* ---- rectangles ---------------------------------------------------------- */

static int rect_empty(struct rect r) { return r.x1 <= r.x0 || r.y1 <= r.y0; }

static struct rect rect_isect(struct rect a, struct rect b)
{
	struct rect r = { a.x0 > b.x0 ? a.x0 : b.x0, a.y0 > b.y0 ? a.y0 : b.y0,
	                  a.x1 < b.x1 ? a.x1 : b.x1, a.y1 < b.y1 ? a.y1 : b.y1 };
	return r;
}

static void rect_add(struct rect *acc, struct rect r)
{
	struct rect s = { 0, 0, scr_w, scr_h };
	r = rect_isect(r, s);
	if (rect_empty(r))
		return;
	if (rect_empty(*acc)) {
		*acc = r;
		return;
	}
	if (r.x0 < acc->x0) acc->x0 = r.x0;
	if (r.y0 < acc->y0) acc->y0 = r.y0;
	if (r.x1 > acc->x1) acc->x1 = r.x1;
	if (r.y1 > acc->y1) acc->y1 = r.y1;
}

static void kick(void)
{
	if (kwm)
		proc_wake(kwm);
}

static void damage(struct rect r)
{
	rect_add(&scene, r);
	rect_add(&present, r);
	kick();
}

static void damage_all(void)
{
	struct rect r = { 0, 0, scr_w, scr_h };
	damage(r);
}

/* One line on the serial console per window life-cycle step:
 *   win: <what> pid=<pid> x=<x> y=<y> w=<w> h=<h>
 * with x, y, w, h describing the client area. The QA scripts read these to
 * know where a window is instead of guessing from screenshots. */
static void trace(const char *what, int pid, int x, int y, int w, int h)
{
	serial_write("win: ");
	serial_write(what);
	serial_write(" pid=");
	serial_write_dec((unsigned int)pid);
	serial_write(" x=");
	if (x < 0) { serial_putc('-'); x = -x; }
	serial_write_dec((unsigned int)x);
	serial_write(" y=");
	if (y < 0) { serial_putc('-'); y = -y; }
	serial_write_dec((unsigned int)y);
	serial_write(" w=");
	serial_write_dec((unsigned int)w);
	serial_write(" h=");
	serial_write_dec((unsigned int)h);
	serial_putc('\n');
}

/* ---- geometry ------------------------------------------------------------ */

static int decorated(const struct win *w) { return !(w->flags & WIN_F_DESKTOP); }

/* The window including its chrome. */
static struct rect outer(const struct win *w)
{
	struct rect r = { w->x, w->y, w->x + w->w, w->y + w->h };
	if (decorated(w)) {
		r.x0 -= WIN_BORDER;
		r.y0 -= WIN_TITLE_H + WIN_BORDER;
		r.x1 += WIN_BORDER;
		r.y1 += WIN_BORDER;
	}
	return r;
}

static struct rect close_box(const struct win *w)
{
	struct rect o = outer(w);
	struct rect r = { o.x1 - WIN_BORDER - CLOSE_W - 3, o.y0 + WIN_BORDER + 3,
	                  o.x1 - WIN_BORDER - 3,           o.y0 + WIN_BORDER + WIN_TITLE_H - 3 };
	return r;
}

static struct rect taskbar_rect(void)
{
	struct rect r = { 0, scr_h - WIN_TASKBAR_H, scr_w, scr_h };
	return r;
}

static int in_rect(struct rect r, int x, int y)
{
	return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1;
}

/* ---- slots --------------------------------------------------------------- */

static int slot_of(int pid)
{
	if (pid <= 0)
		return -1;
	for (int i = 0; i < WIN_MAX; i++)
		if (wins[i].pid == pid)
			return i;
	return -1;
}

static int any_window(void)
{
	for (int i = 0; i < WIN_MAX; i++)
		if (wins[i].pid)
			return 1;
	return 0;
}

int win_has_window(int pid) { return slot_of(pid) >= 0; }

/* Visible windows, back to front. Returns the count. */
static int z_order(int *order)
{
	int n = 0;
	for (int i = 0; i < WIN_MAX; i++)
		if (wins[i].pid && wins[i].visible)
			order[n++] = i;
	for (int i = 1; i < n; i++) {
		int key = order[i], j = i - 1;
		while (j >= 0 && wins[order[j]].z > wins[key].z) {
			order[j + 1] = order[j];
			j--;
		}
		order[j + 1] = key;
	}
	return n;
}

/* ---- events -------------------------------------------------------------- */

static void ev_push(struct win *w, int type, int x, int y, int buttons, int key)
{
	int nw = (w->ev_w + 1) % EVQ;
	/* Motion is a level, not a history: if the newest queued event is
	 * also plain motion, replace it instead of queueing behind it. */
	if (type == WIN_EV_MOUSE && key == 0 && w->ev_w != w->ev_r) {
		struct win_event *last = &w->evq[(w->ev_w + EVQ - 1) % EVQ];
		if (last->type == WIN_EV_MOUSE && last->key == 0) {
			last->x = x;
			last->y = y;
			last->buttons = buttons;
			return;
		}
	}
	if (nw == w->ev_r)
		return;                            /* full: the newest event is lost */
	struct win_event *e = &w->evq[w->ev_w];
	e->type = type;
	e->x = x;
	e->y = y;
	e->buttons = buttons;
	e->key = key;
	w->ev_w = nw;
	if (w->waiter)
		proc_wake(w->waiter);
}

static void key_push(struct win *w, char c)
{
	int nw = (w->key_w + 1) % KEYQ;
	if (nw != w->key_r) {
		w->keyq[w->key_w] = c;
		w->key_w = nw;
	}
	ev_push(w, WIN_EV_KEY, 0, 0, 0, (unsigned char)c);
}

static void scan_push(struct win *w, unsigned int ev)
{
	int nw = (w->scan_w + 1) % KEYQ;
	if (nw != w->scan_r) {
		w->scanq[w->scan_w] = ev;
		w->scan_w = nw;
	}
	ev_push(w, WIN_EV_SCAN, 0, 0, 0, (int)ev);
}

static int has_input(const struct win *w)
{
	return w->ev_r != w->ev_w;
}

/* ---- focus and stacking -------------------------------------------------- */

static void damage_chrome(int i)
{
	if (i < 0 || !wins[i].pid || !wins[i].visible)
		return;
	struct rect o = outer(&wins[i]);
	o.y1 = wins[i].y;                         /* the title bar strip */
	damage(o);
	struct rect all = outer(&wins[i]);        /* and the border colour */
	damage(all);
}

static void set_focus(int i)
{
	if (focus == i)
		return;
	int old = focus;
	focus = i;
	if (i >= 0)
		trace("focus", wins[i].pid, wins[i].x, wins[i].y, wins[i].w, wins[i].h);
	if (old >= 0 && wins[old].pid) {
		ev_push(&wins[old], WIN_EV_FOCUS, 0, 0, 0, 0);
		damage_chrome(old);
	}
	if (i >= 0) {
		ev_push(&wins[i], WIN_EV_FOCUS, 1, 0, 0, 0);
		damage_chrome(i);
	}
	damage(taskbar_rect());
}

static void raise(int i)
{
	if (!decorated(&wins[i]))
		return;                            /* the desktop stays at the bottom */
	wins[i].z = ++z_top;
	damage(outer(&wins[i]));
}

/* Focus the topmost visible window (after the focused one went away). */
static void focus_top(void)
{
	int order[WIN_MAX];
	int n = z_order(order);
	focus = -1;
	set_focus(n ? order[n - 1] : -1);
}

/* ---- back buffer drawing ------------------------------------------------- */

static void bb_fill(struct rect r, struct rect clip, unsigned int rgb)
{
	r = rect_isect(r, clip);
	for (int y = r.y0; y < r.y1; y++) {
		unsigned int *row = back + y * scr_w;
		for (int x = r.x0; x < r.x1; x++)
			row[x] = rgb;
	}
}

static void bb_text(int x, int y, const char *s, struct rect clip, unsigned int rgb)
{
	for (; *s; s++, x += 8) {
		if (x + 8 <= clip.x0 || x >= clip.x1)
			continue;
		const unsigned char *g = font8x8[(unsigned char)*s & 0x7F];
		for (int row = 0; row < 8; row++) {
			int py = y + row;
			if (py < clip.y0 || py >= clip.y1)
				continue;
			for (int col = 0; col < 8; col++) {
				int px = x + col;
				if (px < clip.x0 || px >= clip.x1)
					continue;
				if (g[row] & (0x80 >> col))
					back[py * scr_w + px] = rgb;
			}
		}
	}
}

/* Copy the part of a window's client pixels that falls inside `clip`. The
 * client buffer is w*h XRGB words spread over 4 KiB frames (1024 pixels
 * each), reached through the direct map. */
static void bb_client(const struct win *w, struct rect clip)
{
	struct rect c = { w->x, w->y, w->x + w->w, w->y + w->h };
	c = rect_isect(c, clip);
	for (int y = c.y0; y < c.y1; y++) {
		unsigned int idx = (unsigned int)(y - w->y) * (unsigned int)w->w +
		                   (unsigned int)(c.x0 - w->x);
		unsigned int *dst = back + y * scr_w + c.x0;
		int left = c.x1 - c.x0;
		while (left > 0) {
			unsigned int fi = idx >> 10, off = idx & 1023u;
			if ((int)fi >= w->n_frames)
				return;
			const unsigned int *src = (const unsigned int *)P2V(w->frames[fi]) + off;
			int n = (int)(1024u - off);
			if (n > left)
				n = left;
			for (int i = 0; i < n; i++)
				dst[i] = src[i];
			dst += n;
			idx += (unsigned int)n;
			left -= n;
		}
	}
}

static void draw_chrome(int i, struct rect clip)
{
	const struct win *w = &wins[i];
	struct rect o = outer(w);
	int focused = (i == focus);

	bb_fill(o, clip, focused ? C_BORDER_FOCUS : C_BORDER);

	struct rect bar = { o.x0 + WIN_BORDER, o.y0 + WIN_BORDER,
	                    o.x1 - WIN_BORDER, o.y0 + WIN_BORDER + WIN_TITLE_H };
	bb_fill(bar, clip, focused ? C_TITLE_FOCUS : C_TITLE_BG);

	/* Title, cut off before the close box. */
	struct rect cb = close_box(w);
	struct rect tclip = rect_isect(clip, bar);
	if (tclip.x1 > cb.x0 - 6)
		tclip.x1 = cb.x0 - 6;
	if (focused) {
		struct rect mark = { bar.x0 + 8, bar.y0 + 9, bar.x0 + 12, bar.y0 + 15 };
		bb_fill(mark, clip, C_ACCENT);
	}
	bb_text(bar.x0 + 20, bar.y0 + 8, w->title, tclip,
	        focused ? C_TITLE_FG_F : C_TITLE_FG);

	if (hover_close == i)
		bb_fill(cb, clip, C_CLOSE_HOVER);
	bb_text(cb.x0 + (CLOSE_W - 8) / 2, cb.y0 + (cb.y1 - cb.y0 - 8) / 2, "x", clip,
	        (hover_close == i || focused) ? C_TITLE_FG_F : C_TITLE_FG);

	if (w->flags & WIN_F_RESIZABLE) {          /* corner ticks */
		for (int k = 0; k < 3; k++) {
			struct rect t = { o.x1 - 5 - 4 * k, o.y1 - 3, o.x1 - 3 - 4 * k, o.y1 - 1 };
			bb_fill(t, clip, focused ? C_ACCENT : C_TITLE_FG);
		}
	}
}

static int task_button(int i, struct rect *r)
{
	/* Buttons follow slot order so they do not jump around when the
	 * stacking order changes. Returns 0 when `i` has no button. */
	int n = 0;
	for (int k = 0; k < WIN_MAX; k++) {
		if (!wins[k].pid || !wins[k].visible || !decorated(&wins[k]))
			continue;
		if (k == i) {
			int x = TASK_BTN_X0 + n * (TASK_BTN_W + TASK_BTN_GAP);
			if (x + TASK_BTN_W > scr_w - 64)
				return 0;                  /* no room left before the clock */
			r->x0 = x;
			r->x1 = x + TASK_BTN_W;
			r->y0 = scr_h - WIN_TASKBAR_H + 4;
			r->y1 = scr_h - 4;
			return 1;
		}
		n++;
	}
	return 0;
}

static void draw_taskbar(struct rect clip)
{
	struct rect bar = taskbar_rect();
	if (rect_empty(rect_isect(bar, clip)))
		return;
	bb_fill(bar, clip, C_BAR_BG);
	struct rect line = { 0, bar.y0, scr_w, bar.y0 + 1 };
	bb_fill(line, clip, C_BAR_LINE);
	bb_text(12, bar.y0 + 10, "StinkOS", clip, C_ACCENT);

	for (int i = 0; i < WIN_MAX; i++) {
		struct rect b;
		if (!task_button(i, &b))
			continue;
		bb_fill(b, clip, i == focus ? C_BTN_FOCUS : C_BTN_BG);
		if (i == focus) {
			struct rect u = { b.x0, b.y1 - 2, b.x1, b.y1 };
			bb_fill(u, clip, C_ACCENT);
		}
		struct rect tc = rect_isect(clip, b);
		tc.x1 = tc.x1 < b.x1 - 6 ? tc.x1 : b.x1 - 6;
		bb_text(b.x0 + 8, b.y0 + 6, wins[i].title[0] ? wins[i].title : "(untitled)",
		        tc, i == focus ? C_TITLE_FG_F : C_TITLE_FG);
	}
	bb_text(scr_w - 52, bar.y0 + 10, clock_txt, clip, C_TITLE_FG);
}

static void compose(struct rect clip)
{
	int order[WIN_MAX];
	int n = z_order(order);

	/* Skip the background fill when an opaque desktop window covers it. */
	int covered = 0;
	for (int k = 0; k < n; k++) {
		const struct win *w = &wins[order[k]];
		if (!decorated(w) && w->x <= clip.x0 && w->y <= clip.y0 &&
		    w->x + w->w >= clip.x1 && w->y + w->h >= clip.y1)
			covered = 1;
	}
	if (!covered)
		bb_fill(clip, clip, C_DESKTOP);

	for (int k = 0; k < n; k++) {
		int i = order[k];
		if (rect_empty(rect_isect(outer(&wins[i]), clip)))
			continue;
		if (decorated(&wins[i]))
			draw_chrome(i, clip);
		bb_client(&wins[i], clip);
	}

	if (rsz >= 0) {                            /* rubber band at the pending size */
		const struct win *w = &wins[rsz];
		struct rect o = outer(w);
		int x1 = w->x + rsz_w + WIN_BORDER, y1 = w->y + rsz_h + WIN_BORDER;
		struct rect t = { o.x0, o.y0, x1, o.y0 + 2 }, b = { o.x0, y1 - 2, x1, y1 };
		struct rect l = { o.x0, o.y0, o.x0 + 2, y1 }, r = { x1 - 2, o.y0, x1, y1 };
		bb_fill(t, clip, C_BAND);
		bb_fill(b, clip, C_BAND);
		bb_fill(l, clip, C_BAND);
		bb_fill(r, clip, C_BAND);
	}
	draw_taskbar(clip);
}

/* ---- cursor -------------------------------------------------------------- */

#define CUR_W 12
#define CUR_H 18
static const char *const cursor_shape[CUR_H] = {
	"#           ",
	"##          ",
	"#o#         ",
	"#oo#        ",
	"#ooo#       ",
	"#oooo#      ",
	"#ooooo#     ",
	"#oooooo#    ",
	"#ooooooo#   ",
	"#oooooooo#  ",
	"#ooooooooo# ",
	"#oooooo#####",
	"#ooo#oo#    ",
	"#oo# #oo#   ",
	"#o#  #oo#   ",
	"##    #oo#  ",
	"#     #oo#  ",
	"       ##   ",
};

static struct rect cursor_rect(int x, int y)
{
	struct rect r = { x, y, x + CUR_W, y + CUR_H };
	return r;
}

static void cursor_draw(int x, int y)
{
	for (int r = 0; r < CUR_H; r++)
		for (int c = 0; c < CUR_W; c++) {
			char p = cursor_shape[r][c];
			if (p == ' ')
				continue;
			fb_putpixel((unsigned int)(x + c), (unsigned int)(y + r),
			            p == '#' ? 0x000000u : 0xFFFFFFu);
		}
	cur_drawn = 1;
	cur_x = x;
	cur_y = y;
}

/* ---- rendering ----------------------------------------------------------- */

/* ---- measurement ---------------------------------------------------------
 * What drawing costs, on the serial console:
 *
 *   win: perf frames=F composed=C copied=P busy_us=B worst_us=W
 *
 * for each second in which at least one frame was drawn: how many frames,
 * how many pixels were recomposed into the back buffer and how many were
 * copied to video memory (both in thousands), the time spent doing it and
 * the longest single frame. A still screen prints nothing, which is the
 * point: there is no work when nothing changed. Because the kernel does
 * not preempt itself, the longest frame is also the longest this thread
 * keeps the CPU from a process that became runnable (interrupts stay on
 * throughout). Times come from the time-stamp counter, calibrated against
 * the timer; they read 0 on a CPU without one. */
static struct {
	unsigned int frames, composed, copied;     /* this second; pixels */
	unsigned int busy_us, worst_us;
	unsigned int since;                        /* tick this second began */
} perf;
static unsigned int tsc_per_us;

static unsigned int tsc_lo(void)
{
	unsigned int lo, hi;
	__asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
	(void)hi;                                  /* intervals here are far below 2^32 cycles */
	return lo;
}

static void perf_calibrate(void)
{
	if (!cpuid_has_tsc())
		return;
	unsigned int t0 = pit_ticks();
	while (pit_ticks() == t0)                  /* start on a tick edge */
		proc_block(1);
	t0 = pit_ticks();
	unsigned int c0 = tsc_lo();
	while (pit_ticks() - t0 < 10)
		proc_block(1);
	unsigned int c1 = tsc_lo();
	tsc_per_us = (c1 - c0) / ((pit_ticks() - t0) * 10000u);
}

static void perf_report(void)
{
	unsigned int now = pit_ticks();
	if (now - perf.since < 100)
		return;
	if (perf.frames) {
		serial_write("win: perf frames=");
		serial_write_dec(perf.frames);
		serial_write(" composed=");
		serial_write_dec(perf.composed / 1000u);
		serial_write(" copied=");
		serial_write_dec(perf.copied / 1000u);
		serial_write(" busy_us=");
		serial_write_dec(perf.busy_us);
		serial_write(" worst_us=");
		serial_write_dec(perf.worst_us);
		serial_putc('\n');
	}
	perf.frames = perf.composed = perf.copied = 0;
	perf.busy_us = perf.worst_us = 0;
	perf.since = now;
}

static unsigned int rect_area(struct rect r)
{
	return rect_empty(r) ? 0 : (unsigned int)(r.x1 - r.x0) * (unsigned int)(r.y1 - r.y0);
}

static void render(void)
{
	unsigned int c0 = tsc_per_us ? tsc_lo() : 0;

	perf.frames++;
	perf.composed += rect_area(scene);
	perf.copied += rect_area(present);
	if (!rect_empty(scene)) {
		compose(scene);
		scene.x0 = scene.x1 = 0;
	}
	if (!rect_empty(present)) {
		fb_present(back, (unsigned int)present.x0, (unsigned int)present.y0,
		           (unsigned int)(present.x1 - present.x0),
		           (unsigned int)(present.y1 - present.y0));
		present.x0 = present.x1 = 0;
		cursor_draw(last_mx, last_my);         /* the copy may have painted over it */
	}

	if (tsc_per_us) {
		unsigned int us = (tsc_lo() - c0) / tsc_per_us;
		perf.busy_us += us;
		if (us > perf.worst_us)
			perf.worst_us = us;
	}
}

/* ---- input --------------------------------------------------------------- */

enum { PART_NONE, PART_CLIENT, PART_TITLE, PART_CLOSE, PART_GRIP };

/* Topmost visible window under (x, y) and which part of it. */
static int hit(int x, int y, int *part)
{
	int order[WIN_MAX];
	int n = z_order(order);
	for (int k = n - 1; k >= 0; k--) {
		const struct win *w = &wins[order[k]];
		struct rect o = outer(w);
		if (!in_rect(o, x, y))
			continue;
		if (!decorated(w)) {
			*part = PART_CLIENT;
		} else if (in_rect(close_box(w), x, y)) {
			*part = PART_CLOSE;
		} else if ((w->flags & WIN_F_RESIZABLE) && x >= o.x1 - GRIP && y >= o.y1 - GRIP) {
			*part = PART_GRIP;
		} else if (y < w->y) {
			*part = PART_TITLE;
		} else if (x >= w->x && x < w->x + w->w && y < w->y + w->h) {
			*part = PART_CLIENT;
		} else {
			*part = PART_TITLE;                /* the 1 px border behaves like the frame */
		}
		return order[k];
	}
	*part = PART_NONE;
	return -1;
}

static void set_hover_close(int i)
{
	if (hover_close == i)
		return;
	int old = hover_close;
	hover_close = i;
	if (old >= 0 && wins[old].pid && wins[old].visible)
		damage(close_box(&wins[old]));
	if (i >= 0)
		damage(close_box(&wins[i]));
}

static void mouse_step(void)
{
	int mx, my;
	unsigned char mb;
	mouse_get_state(&mx, &my, &mb);
	int moved = (mx != last_mx || my != last_my);
	int changed = mb ^ last_mb;                /* buttons that flipped */
	int down = (mb & 1) && !(last_mb & 1);
	int up = !(mb & 1) && (last_mb & 1);

	if (moved || !cur_drawn) {
		if (cur_drawn)
			rect_add(&present, cursor_rect(cur_x, cur_y));
		rect_add(&present, cursor_rect(mx, my));
	}
	last_mx = mx;
	last_my = my;
	last_mb = mb;
	if (!moved && !changed)
		return;

	/* Moving a window: follow the pointer until the button comes up. */
	if (drag >= 0) {
		struct win *w = &wins[drag];
		if (mb & 1) {
			int nx = mx - drag_dx, ny = my - drag_dy;
			int top = WIN_TITLE_H + WIN_BORDER;
			/* Keep a grabbable piece of the title bar on screen. */
			if (nx > scr_w - 48)          nx = scr_w - 48;
			if (nx < 48 - w->w)           nx = 48 - w->w;
			if (ny < top)                 ny = top;
			if (ny > scr_h - WIN_TASKBAR_H - 4) ny = scr_h - WIN_TASKBAR_H - 4;
			if (nx != w->x || ny != w->y) {
				damage(outer(w));
				w->x = nx;
				w->y = ny;
				damage(outer(w));
			}
		}
		if (up) {
			trace("moved", w->pid, w->x, w->y, w->w, w->h);
			drag = -1;
		}
		return;
	}

	/* Resizing: only the rubber band moves; the app is told on release. */
	if (rsz >= 0) {
		struct win *w = &wins[rsz];
		if (mb & 1) {
			int nw = mx - w->x, nh = my - w->y;
			int max_w = scr_w - 2 * WIN_BORDER;
			int max_h = scr_h - WIN_TASKBAR_H - WIN_TITLE_H - 2 * WIN_BORDER;
			if (nw < WIN_MIN_W) nw = WIN_MIN_W;
			if (nh < WIN_MIN_H) nh = WIN_MIN_H;
			if (nw > max_w)     nw = max_w;
			if (nh > max_h)     nh = max_h;
			if (nw != rsz_w || nh != rsz_h) {
				struct rect o = outer(w);
				struct rect a = { o.x0, o.y0, w->x + (rsz_w > nw ? rsz_w : nw) + WIN_BORDER,
				                  w->y + (rsz_h > nh ? rsz_h : nh) + WIN_BORDER };
				rsz_w = nw;
				rsz_h = nh;
				damage(a);
			}
		}
		if (up) {
			struct rect o = outer(w);
			struct rect a = { o.x0, o.y0, w->x + rsz_w + WIN_BORDER, w->y + rsz_h + WIN_BORDER };
			int i = rsz;
			rsz = -1;
			damage(a);
			if (rsz_w != wins[i].w || rsz_h != wins[i].h)
				ev_push(&wins[i], WIN_EV_RESIZE, rsz_w, rsz_h, 0, 0);
		}
		return;
	}

	/* A client that received a press keeps the pointer until every button
	 * is released, even when it leaves the window. */
	if (grab >= 0) {
		struct win *w = &wins[grab];
		ev_push(w, WIN_EV_MOUSE, mx - w->x, my - w->y, mb, changed);
		if (!mb)
			grab = -1;
		return;
	}

	int part;
	int i = hit(mx, my, &part);
	set_hover_close(part == PART_CLOSE ? i : -1);

	if (my >= scr_h - WIN_TASKBAR_H) {
		if (down) {
			for (int k = 0; k < WIN_MAX; k++) {
				struct rect b;
				if (task_button(k, &b) && in_rect(b, mx, my)) {
					raise(k);
					set_focus(k);
					break;
				}
			}
		}
		return;
	}
	if (i < 0)
		return;

	struct win *w = &wins[i];
	if (changed & mb) {                        /* some button went down */
		set_focus(i);
		raise(i);
		if (part == PART_CLIENT) {
			grab = i;
			ev_push(w, WIN_EV_MOUSE, mx - w->x, my - w->y, mb, changed);
		} else if (down && part == PART_CLOSE) {
			ev_push(w, WIN_EV_CLOSE, 0, 0, 0, 0);
		} else if (down && part == PART_TITLE) {
			drag = i;
			drag_dx = mx - w->x;
			drag_dy = my - w->y;
		} else if (down && part == PART_GRIP) {
			rsz = i;
			rsz_w = w->w;
			rsz_h = w->h;
		}
	} else if (part == PART_CLIENT) {
		ev_push(w, WIN_EV_MOUSE, mx - w->x, my - w->y, mb, changed);   /* hover */
	}
}

static void keyboard_step(void)
{
	char c;
	unsigned int ev;
	struct win *w = (focus >= 0 && wins[focus].pid) ? &wins[focus] : 0;
	while ((c = keyboard_getchar()) != 0)
		if (w)
			key_push(w, c);
	while ((ev = keyboard_get_event()) != 0)
		if (w)
			scan_push(w, ev);
}

static void clock_step(void)
{
	unsigned int now = pit_ticks();
	if (clock_tick && now - clock_tick < 100)
		return;
	clock_tick = now ? now : 1;
	struct rtc_time t;
	rtc_read(&t);
	char txt[6] = { (char)('0' + (t.hour / 10) % 10), (char)('0' + t.hour % 10), ':',
	                (char)('0' + (t.minute / 10) % 10), (char)('0' + t.minute % 10), 0 };
	int same = 1;
	for (int i = 0; i < 6; i++)
		if (txt[i] != clock_txt[i])
			same = 0;
	if (same)
		return;
	for (int i = 0; i < 6; i++)
		clock_txt[i] = txt[i];
	struct rect r = { scr_w - 60, scr_h - WIN_TASKBAR_H, scr_w, scr_h };
	damage(r);
}

/* ---- the thread ---------------------------------------------------------- */

static void kwm_main(void)
{
	perf_calibrate();
	for (;;) {
		if (!any_window()) {                   /* text menu / legacy mode: stay out of the way */
			cur_drawn = 0;
			proc_block(20);
			continue;
		}
		if (!fs_pid) {
			mouse_step();
			keyboard_step();
			clock_step();
			/* At most one frame per tick, however often we are woken. */
			if (pit_ticks() != frame_tick &&
			    (!rect_empty(scene) || !rect_empty(present))) {
				render();
				frame_tick = pit_ticks();
			}
			perf_report();
		}
		proc_block(1);
	}
}

void win_irq_wake(void)
{
	if (kwm)
		proc_wake(kwm);
}

void win_init(void)
{
	if (!fb_usable())
		return;
	scr_w = (int)fb_width();
	scr_h = (int)fb_height();
	unsigned int pages = ((unsigned int)(scr_w * scr_h) * 4u + 4095u) / 4096u;
	unsigned int phys = pmm_alloc_contig(pages);
	if (!phys) {
		serial_write("win: no memory for the back buffer, compositor disabled\n");
		return;
	}
	back = (unsigned int *)P2V(phys);
	kwm = proc_kthread_create("kwm", kwm_main);
	if (!kwm) {
		back = 0;
		return;
	}
	kwm->priority = 8;                         /* ahead of ordinary processes (16) */
	serial_write("win: compositor ready\n");
}

/* ---- syscall backends ---------------------------------------------------- */

static void release_frames(struct win *w)
{
	for (int i = 0; i < w->n_frames; i++)
		pmm_free(w->frames[i]);
	w->n_frames = 0;
}

/* Allocate `n` zeroed frames into `out`. All or nothing. */
static int alloc_frames(unsigned int *out, int n)
{
	for (int i = 0; i < n; i++) {
		unsigned int fr = pmm_alloc();
		if (!fr) {
			for (int j = 0; j < i; j++)
				pmm_free(out[j]);
			return -1;
		}
		unsigned int *p = (unsigned int *)P2V(fr);
		for (int k = 0; k < 1024; k++)
			p[k] = 0;
		out[i] = fr;
	}
	return 0;
}

static int size_ok(unsigned int w, unsigned int h, unsigned int flags)
{
	unsigned int max_w = (unsigned int)scr_w, max_h = (unsigned int)scr_h;
	if (!(flags & WIN_F_DESKTOP)) {
		max_w -= 2 * WIN_BORDER;
		max_h -= WIN_TASKBAR_H + WIN_TITLE_H + 2 * WIN_BORDER;
		if (w < WIN_MIN_W || h < WIN_MIN_H)
			return 0;
	}
	return w >= 1 && h >= 1 && w <= max_w && h <= max_h;
}

int win_create(int pid, unsigned int w, unsigned int h, unsigned int flags)
{
	if (!back || pid <= 0 || slot_of(pid) >= 0)
		return -1;
	if (flags & ~(WIN_F_DESKTOP | WIN_F_RESIZABLE))
		return -1;
	if (!size_ok(w, h, flags))
		return -1;

	int si = -1;
	for (int i = 0; i < WIN_MAX; i++)
		if (!wins[i].pid) {
			si = i;
			break;
		}
	if (si < 0)
		return -1;
	struct win *s = &wins[si];

	int need = (int)((w * h * 4u + 4095u) / 4096u);
	if (alloc_frames(s->frames, need) != 0)
		return -1;
	/* The buffer is part of the contract: a window whose pixels are not
	 * fully mapped does not exist. */
	if (paging_map_shared(USER_WIN_BASE, s->frames, need) != 0) {
		for (int i = 0; i < need; i++)
			pmm_free(s->frames[i]);
		return -1;
	}
	s->n_frames = need;
	s->pid      = pid;
	s->w        = (int)w;
	s->h        = (int)h;
	s->x        = 0;
	s->y        = 0;
	s->flags    = flags;
	s->visible  = 0;
	s->z        = 0;
	s->title[0] = 0;
	s->ev_r = s->ev_w = s->key_r = s->key_w = s->scan_r = s->scan_w = 0;
	s->waiter   = 0;
	return 0;
}

int win_show(int pid, int x, int y, const char *title)
{
	int si = slot_of(pid);
	if (si < 0)
		return -1;
	struct win *s = &wins[si];
	if (title) {
		int i = 0;
		for (; i < (int)sizeof(s->title) - 1 && title[i]; i++)
			s->title[i] = title[i];
		s->title[i] = 0;
	}
	if (!decorated(s)) {
		s->x = 0;
		s->y = 0;
	} else {
		int top = WIN_TITLE_H + WIN_BORDER;
		if (x < 0 || y < 0) {                  /* let the kernel place it: cascade */
			x = 72 + 28 * si;
			y = 56 + 28 * si;
		}
		/* x, y name the outer top-left corner. */
		s->x = x + WIN_BORDER;
		s->y = y + top;
		if (s->x + s->w + WIN_BORDER > scr_w) s->x = scr_w - s->w - WIN_BORDER;
		if (s->y + s->h + WIN_BORDER > scr_h - WIN_TASKBAR_H)
			s->y = scr_h - WIN_TASKBAR_H - s->h - WIN_BORDER;
		if (s->x < WIN_BORDER) s->x = WIN_BORDER;
		if (s->y < top)        s->y = top;
	}
	if (!s->visible) {
		s->visible = 1;
		if (decorated(s))
			s->z = ++z_top;
		set_focus(si);
	}
	trace("show", pid, s->x, s->y, s->w, s->h);
	damage(outer(s));
	damage(taskbar_rect());
	return 0;
}

void win_flush(int pid)
{
	int si = slot_of(pid);
	if (si < 0 || !wins[si].visible)
		return;
	struct win *s = &wins[si];
	struct rect r = { s->x, s->y, s->x + s->w, s->y + s->h };
	damage(r);
}

/* Remove slot `si` from the screen and free its pixels. `unmap` is set
 * when running in the owner's address space. */
static void destroy_slot(int si, int unmap)
{
	struct win *s = &wins[si];
	if (!s->pid)
		return;
	struct rect o = outer(s);
	int was_visible = s->visible;
	trace("destroy", s->pid, s->x, s->y, s->w, s->h);

	if (unmap)
		paging_unmap_shared(USER_WIN_BASE, s->n_frames);
	release_frames(s);
	if (s->waiter)
		proc_wake(s->waiter);
	s->pid = 0;
	s->visible = 0;
	s->waiter = 0;

	if (grab == si)        grab = -1;
	if (drag == si)        drag = -1;
	if (rsz == si)         rsz = -1;
	if (hover_close == si) hover_close = -1;
	if (focus == si)
		focus_top();
	if (was_visible) {
		damage(o);
		damage(taskbar_rect());
	}
}

void win_destroy(int pid)
{
	int si = slot_of(pid);
	if (si >= 0)
		destroy_slot(si, 1);
}

int win_get_event(int pid, struct win_event *ev)
{
	int si = slot_of(pid);
	if (si < 0 || !ev)
		return -1;
	struct win *s = &wins[si];
	if (s->ev_r == s->ev_w)
		return -1;
	*ev = s->evq[s->ev_r];
	s->ev_r = (s->ev_r + 1) % EVQ;
	return 0;
}

int win_wait_event(int pid, unsigned int timeout_ticks)
{
	int si = slot_of(pid);
	if (si < 0)
		return -1;
	struct win *s = &wins[si];
	if (!has_input(s) && timeout_ticks) {
		s->waiter = proc_current();
		proc_block(timeout_ticks);
		/* The slot may have been torn down while we slept. */
		if (wins[si].pid != pid)
			return -1;
		s->waiter = 0;
	}
	return has_input(s) ? 1 : 0;
}

void win_raise(int pid)
{
	int si = slot_of(pid);
	if (si < 0 || !wins[si].visible)
		return;
	raise(si);
	set_focus(si);
}

void win_move(int pid, int x, int y)
{
	int si = slot_of(pid);
	if (si < 0 || !decorated(&wins[si]))
		return;
	struct win *s = &wins[si];
	int top = WIN_TITLE_H + WIN_BORDER;
	if (s->visible)
		damage(outer(s));
	s->x = x + WIN_BORDER;
	s->y = y + top;
	if (s->x > scr_w - 48)                 s->x = scr_w - 48;
	if (s->x < 48 - s->w)                  s->x = 48 - s->w;
	if (s->y < top)                        s->y = top;
	if (s->y > scr_h - WIN_TASKBAR_H - 4)  s->y = scr_h - WIN_TASKBAR_H - 4;
	if (s->visible)
		damage(outer(s));
}

int win_resize(int pid, unsigned int w, unsigned int h)
{
	/* Too large for a kernel stack; one caller at a time by construction. */
	static unsigned int fresh[WIN_MAX_FRAMES];

	int si = slot_of(pid);
	if (si < 0)
		return -1;
	struct win *s = &wins[si];
	if (!size_ok(w, h, s->flags))
		return -1;
	if ((int)w == s->w && (int)h == s->h)
		return 0;

	int need = (int)((w * h * 4u + 4095u) / 4096u);
	if (alloc_frames(fresh, need) != 0)
		return -1;                             /* the old buffer is untouched */

	/* Swap the mapping. If the new one cannot be installed, put the old
	 * one back: the caller keeps a working window either way. */
	paging_unmap_shared(USER_WIN_BASE, s->n_frames);
	if (paging_map_shared(USER_WIN_BASE, fresh, need) != 0) {
		for (int i = 0; i < need; i++)
			pmm_free(fresh[i]);
		if (paging_map_shared(USER_WIN_BASE, s->frames, s->n_frames) != 0) {
			/* Cannot happen: the page tables for this range were just
			 * in use. If it does, the window is gone rather than half
			 * mapped. */
			destroy_slot(si, 0);
		}
		return -1;
	}
	struct rect before = outer(s);
	release_frames(s);
	for (int i = 0; i < need; i++)
		s->frames[i] = fresh[i];
	s->n_frames = need;
	s->w = (int)w;
	s->h = (int)h;
	trace("resize", pid, s->x, s->y, s->w, s->h);
	if (s->visible) {
		damage(before);
		damage(outer(s));
	}
	return 0;
}

/* ---- process lifetime ---------------------------------------------------- */

void win_proc_exit(int pid)
{
	int si = slot_of(pid);
	if (si >= 0)
		destroy_slot(si, 1);
	if (fs_pid == pid) {
		trace("fullscreen-end", pid, 0, 0, scr_w, scr_h);
		fs_pid = 0;
		keyboard_flush();                      /* keys meant for it are not for a window */
		mouse_reset_delta();
		cur_drawn = 0;
		damage_all();
	}
}

void win_session_end(void)
{
	for (int i = 0; i < WIN_MAX; i++) {
		if (!wins[i].pid)
			continue;
		struct proc *p = proc_get(wins[i].pid);
		if (p && p->pid > 1)
			proc_kill(p);                      /* exits at its next return to ring 3 */
		/* Not our address space: the frames are freed here and the
		 * owner's PG_SHARED mappings die with it; it never runs user
		 * code again. */
		destroy_slot(i, 0);
	}
	fs_pid = 0;
	focus = -1;
	cur_drawn = 0;
	scene.x0 = scene.x1 = present.x0 = present.x1 = 0;
}

/* ---- legacy programs ----------------------------------------------------- */

int win_legacy_draw(int pid)
{
	if (!back)
		return 1;                              /* no compositor: plain framebuffer */
	if (slot_of(pid) >= 0)
		return 0;                              /* window owners draw into their buffer */
	if (!fs_pid && any_window()) {
		/* First direct draw by a windowless program while the desktop
		 * is up: it takes the screen until it exits. */
		fs_pid = pid;
		trace("fullscreen", pid, 0, 0, scr_w, scr_h);
		grab = drag = rsz = -1;
		cur_drawn = 0;
		keyboard_flush();
		mouse_reset_delta();
		fb_rect(0, 0, (unsigned int)scr_w, (unsigned int)scr_h, 0x000000);
	}
	return 1;
}

int win_getkey(int pid, int *out)
{
	int si = slot_of(pid);
	if (si < 0)
		return 0;
	struct win *s = &wins[si];
	if (s->key_r == s->key_w) {
		*out = 0;
	} else {
		*out = (unsigned char)s->keyq[s->key_r];
		s->key_r = (s->key_r + 1) % KEYQ;
	}
	return 1;
}

int win_getkeyevent(int pid, unsigned int *out)
{
	int si = slot_of(pid);
	if (si < 0)
		return 0;
	struct win *s = &wins[si];
	if (s->scan_r == s->scan_w) {
		*out = 0;
	} else {
		*out = s->scanq[s->scan_r];
		s->scan_r = (s->scan_r + 1) % KEYQ;
	}
	return 1;
}
