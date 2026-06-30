/* win.h — kernel window table and compositor for StinkOS.
 *
 * Each user process can own one window. The compositor blits window pixel
 * buffers to the physical framebuffer in z-order every ~33ms (30fps).
 *
 * User apps write to USER_WIN_BASE (memlayout.h), call sys_win_flush()
 * to signal a repaint, and poll events with sys_win_get_event().
 */
#ifndef KERNEL_SYS_WIN_H
#define KERNEL_SYS_WIN_H

/* Maximum windows simultaneously on screen. */
#define WIN_MAX        8

/* Compositor taskbar reserved at the bottom of the screen. */
#define TASKBAR_H      24

/* Maximum 4KiB frames per window buffer.
 * 768 × 4096 = 3,145,728 bytes → supports 1024×768 at 32bpp. */
#define WIN_MAX_FRAMES 768

/* Input event types */
#define WIN_EV_NONE    0
#define WIN_EV_MOUSE   1
#define WIN_EV_KEY     2
#define WIN_EV_CLOSE   3

/* Per-event capacity per window (circular queue). */
#define WIN_EV_CAP     16

struct win_event {
    int type;     /* WIN_EV_* */
    int x, y;    /* mouse: window-relative coords */
    int buttons;  /* mouse button bitmask */
    int key;      /* key code (WIN_EV_KEY only) */
};

struct win_slot {
    int          pid;                      /* owning process (0 = free) */
    unsigned int w, h;                     /* pixel dimensions */
    int          x, y;                     /* top-left screen position */
    int          z;                        /* z-order (higher = in front) */
    int          visible;                  /* 1 = show during composite */
    int          dirty;                    /* 1 = needs repaint */
    char         title[64];               /* window title */
    unsigned int frames[WIN_MAX_FRAMES];   /* physical page frames for buffer */
    int          n_frames;                 /* frames allocated */
    struct win_event evq[WIN_EV_CAP];     /* input event queue */
    int          ev_r, ev_w;              /* circular queue read/write indices */
};

/* Initialise window subsystem (call once from kernel main). */
void win_init(void);

/* Called every timer tick from irq_handler; composites at ~30fps. */
void win_tick(void);

/* Create a window for the current process: allocates 'n_frames' physical
 * pages, maps them at USER_WIN_BASE in the caller's VAS, fills slot.
 * Returns 0 on success, -1 on failure. */
int win_create(int pid, unsigned int w, unsigned int h);

/* Set position/title and mark window visible. */
int win_show(int pid, int x, int y, const char *title);

/* Hide window without destroying it. */
int win_hide(int pid);

/* Mark dirty and schedule composite on next tick. */
void win_flush(int pid);

/* Destroy window, unmap pages, free frames. */
void win_destroy(int pid);

/* Pop one input event. Returns 0 if event written to *ev, -1 if queue empty. */
int win_get_event(int pid, struct win_event *ev);

/* Bring window to front (max z). */
void win_raise(int pid);

/* Move window to new screen position. */
void win_move(int pid, int x, int y);

/* Force a full composite of all visible windows to the framebuffer. */
void win_composite(void);

/* Redirect drawing syscalls to a process's window buffer.
 * Returns 1 if the pixel/rect/blit was written to the window buffer,
 * 0 if the process has no window (caller should write to FB). */
int win_redirect_putpixel(int pid, unsigned int x, unsigned int y, unsigned int rgb);
int win_redirect_fillrect(int pid, unsigned int x, unsigned int y,
                          unsigned int w, unsigned int h, unsigned int rgb);
int win_redirect_blit(int pid, unsigned int x, unsigned int y,
                      unsigned int w, unsigned int h,
                      const unsigned int *src);

#endif
