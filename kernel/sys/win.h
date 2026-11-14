/* win.h -- kernel window manager and compositor.
 *
 * A process may own one window. Its pixels live in page frames the window
 * owns, mapped read-write into the process at USER_WIN_BASE: the app draws
 * with plain memory stores and calls SYS_WIN_FLUSH when a frame is ready.
 *
 * The kernel draws everything around the client area (title bar, close
 * box, border, taskbar, cursor) and decides where input goes. All of it
 * happens in one kernel thread, `kwm`; see the top of win.c for the model.
 */
#ifndef KERNEL_SYS_WIN_H
#define KERNEL_SYS_WIN_H

#define WIN_MAX          8        /* windows that can exist at once */
#define WIN_MAX_FRAMES   768      /* 768 * 4 KiB = 3 MiB = 1024x768 XRGB */
#define WIN_MIN_W        120
#define WIN_MIN_H        60

/* Chrome the kernel draws around a decorated client area. */
#define WIN_TITLE_H      24
#define WIN_BORDER       1
#define WIN_TASKBAR_H    28

/* SYS_WIN_CREATE flags. */
#define WIN_F_DESKTOP    0x1u     /* no chrome, always at the bottom, not in the taskbar */
#define WIN_F_RESIZABLE  0x2u     /* the user may drag the bottom-right corner */

/* Event types (struct win_event.type). */
#define WIN_EV_NONE      0
#define WIN_EV_MOUSE     1   /* x, y = client coords; buttons = state; key = buttons that changed */
#define WIN_EV_KEY       2   /* key = character, same codes SYS_GETKEY returns */
#define WIN_EV_CLOSE     3   /* the user clicked the close box */
#define WIN_EV_RESIZE    4   /* x, y = requested client size; answer with SYS_WIN_RESIZE */
#define WIN_EV_SCAN      5   /* key = raw key event, same packing SYS_GETKEYEVENT returns */
#define WIN_EV_FOCUS     6   /* x = 1 gained, 0 lost */

struct win_event {
	int type;
	int x, y;
	int buttons;
	int key;
};

struct regs;

/* Boot: reserve the back buffer and start the kwm thread. */
void win_init(void);

/* Interrupt context: input arrived, wake the compositor thread. */
void win_irq_wake(void);

/* ---- syscall backends (process context, act on the caller's window) ---- */
int  win_create(int pid, unsigned int w, unsigned int h, unsigned int flags);
int  win_show(int pid, int x, int y, const char *title);
void win_flush(int pid);
void win_destroy(int pid);
int  win_get_event(int pid, struct win_event *ev);
int  win_wait_event(int pid, unsigned int timeout_ticks);
void win_raise(int pid);
void win_move(int pid, int x, int y);
int  win_resize(int pid, unsigned int w, unsigned int h);

/* ---- process lifetime ---- */

/* The process is going away or replacing its image: drop its window and
 * give the screen back if it had taken it over. */
void win_proc_exit(int pid);

/* The session leader (PID 1's app) ended: every other window owner is
 * killed and the compositor goes idle, so the kernel menu can draw. */
void win_session_end(void);

/* ---- legacy full-screen programs ----
 *
 * A program without a window that draws straight to the framebuffer (the
 * shell, the C games, Doom) takes the whole screen for itself while it
 * runs: the compositor stops drawing and stops consuming input until that
 * process exits. Drawing syscalls call win_legacy_draw() first; it returns
 * 0 when the caller owns a window (it must draw into its buffer instead)
 * and 1 when the direct draw may go ahead. */
int  win_legacy_draw(int pid);

/* Input syscalls for a process that owns a window read that window's own
 * queues, so a background window cannot take keys meant for the focused
 * one. Each returns 1 when it handled the call (result in *out). */
int  win_getkey(int pid, int *out);
int  win_getkeyevent(int pid, unsigned int *out);
int  win_has_window(int pid);

#endif
