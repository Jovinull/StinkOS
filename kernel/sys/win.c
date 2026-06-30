/* win.c — kernel window table and compositor for StinkOS.
 *
 * Physical frames are allocated via pmm_alloc() and accessed from kernel
 * context via P2V(frame) (every frame is within the 256MiB direct map).
 * Those same frames are mapped into the owner process's VAS at USER_WIN_BASE
 * via paging_map_win_buf() so userland can write pixel data directly.
 *
 * Compositor: runs every 3 PIT ticks (~30fps). Iterates visible windows by
 * z-order (back to front), blits each page-aligned buffer segment to the
 * physical FB via fb_blit_row(), then redraws the mouse cursor on top.
 */
#include "win.h"
#include "defs.h"
#include "../arch/memlayout.h"
#include "../arch/paging.h"
#include "../arch/pmm.h"
#include "../drivers/video/fb.h"
#include "../drivers/input/mouse.h"

/* ── State ──────────────────────────────────────────────────────────────── */

static struct win_slot slots[WIN_MAX];
static int focused_slot = -1;    /* index into slots[], -1 = none */

/* ── Init ───────────────────────────────────────────────────────────────── */

void win_init(void)
{
    for (int i = 0; i < WIN_MAX; i++) {
        slots[i].pid = 0;
        slots[i].visible = 0;
        slots[i].dirty   = 0;
        slots[i].n_frames = 0;
        slots[i].ev_r = slots[i].ev_w = 0;
    }
    focused_slot = -1;
}

/* ── Internal helpers ────────────────────────────────────────────────────── */

static int slot_for_pid(int pid)
{
    for (int i = 0; i < WIN_MAX; i++)
        if (slots[i].pid == pid)
            return i;
    return -1;
}

static int alloc_slot(void)
{
    for (int i = 0; i < WIN_MAX; i++)
        if (slots[i].pid == 0)
            return i;
    return -1;
}

static void ev_push(struct win_slot *s, const struct win_event *ev)
{
    int nw = (s->ev_w + 1) % WIN_EV_CAP;
    if (nw == s->ev_r)
        return;  /* queue full, drop event */
    s->evq[s->ev_w] = *ev;
    s->ev_w = nw;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

int win_create(int pid, unsigned int w, unsigned int h)
{
    if (pid <= 0 || w == 0 || h == 0)
        return -1;
    if (w > 4096 || h > 4096)
        return -1;  /* cap to prevent u32 overflow in w*h*4 below */
    if (slot_for_pid(pid) >= 0)
        return -1;  /* process already has a window */

    unsigned int need = (w * h * 4 + 4095) / 4096;
    if (need > WIN_MAX_FRAMES)
        return -1;  /* too large */

    int si = alloc_slot();
    if (si < 0)
        return -1;

    struct win_slot *s = &slots[si];

    /* Allocate physical frames. */
    s->n_frames = 0;
    for (unsigned int i = 0; i < need; i++) {
        unsigned int fr = pmm_alloc();
        if (!fr) {
            /* OOM: release already-allocated frames */
            for (int j = 0; j < s->n_frames; j++)
                pmm_free(s->frames[j]);
            s->n_frames = 0;
            return -1;
        }
        /* Zero the frame so the window starts blank. */
        unsigned int *kp = (unsigned int *)P2V(fr);
        for (unsigned int k = 0; k < 4096 / 4; k++)
            kp[k] = 0;
        s->frames[s->n_frames++] = fr;
    }

    /* Map frames into user VAS at USER_WIN_BASE. */
    paging_map_win_buf(USER_WIN_BASE, s->frames, s->n_frames);

    /* Initialise slot. */
    s->pid     = pid;
    s->w       = w;
    s->h       = h;
    s->x       = 0;
    s->y       = 0;
    s->z       = si;    /* default z = slot index */
    s->visible = 0;
    s->dirty   = 0;
    s->ev_r    = 0;
    s->ev_w    = 0;
    for (int i = 0; i < 64; i++) s->title[i] = 0;

    return 0;
}

int win_show(int pid, int x, int y, const char *title)
{
    int si = slot_for_pid(pid);
    if (si < 0) return -1;
    struct win_slot *s = &slots[si];
    s->x = x;
    s->y = y;
    /* Copy title (up to 63 chars + NUL). */
    if (title) {
        int i;
        for (i = 0; i < 63 && title[i]; i++)
            s->title[i] = title[i];
        s->title[i] = 0;
    }
    s->visible = 1;
    s->dirty   = 1;
    if (focused_slot < 0)
        focused_slot = si;
    return 0;
}

int win_hide(int pid)
{
    int si = slot_for_pid(pid);
    if (si < 0) return -1;
    slots[si].visible = 0;
    if (focused_slot == si) focused_slot = -1;
    return 0;
}

void win_flush(int pid)
{
    int si = slot_for_pid(pid);
    if (si >= 0)
        slots[si].dirty = 1;
}

void win_destroy(int pid)
{
    int si = slot_for_pid(pid);
    if (si < 0) return;
    struct win_slot *s = &slots[si];

    if (focused_slot == si)
        focused_slot = -1;

    /* Unmap from user VAS. */
    paging_unmap_win_buf(USER_WIN_BASE, s->n_frames);

    /* Free physical frames. */
    for (int i = 0; i < s->n_frames; i++)
        pmm_free(s->frames[i]);

    s->pid      = 0;
    s->visible  = 0;
    s->dirty    = 0;
    s->n_frames = 0;
}

int win_get_event(int pid, struct win_event *ev)
{
    int si = slot_for_pid(pid);
    if (si < 0 || !ev) return -1;
    struct win_slot *s = &slots[si];
    if (s->ev_r == s->ev_w) return -1;  /* empty */
    *ev   = s->evq[s->ev_r];
    s->ev_r = (s->ev_r + 1) % WIN_EV_CAP;
    return 0;
}

void win_raise(int pid)
{
    int si = slot_for_pid(pid);
    if (si < 0) return;
    /* Find current max z among visible windows */
    int maxz = 0;
    for (int i = 0; i < WIN_MAX; i++)
        if (slots[i].pid && slots[i].visible && slots[i].z > maxz)
            maxz = slots[i].z;
    slots[si].z = maxz + 1;
    focused_slot = si;
}

void win_move(int pid, int x, int y)
{
    int si = slot_for_pid(pid);
    if (si < 0) return;
    slots[si].x = x;
    slots[si].y = y;
    slots[si].dirty = 1;
}

/* ── Syscall routing (redirect drawing calls to window buffer) ───────────── */

/* Write an ARGB pixel to the window buffer at (x, y). */
static void win_write_px(struct win_slot *s,
                         unsigned int x, unsigned int y, unsigned int argb)
{
    if (x >= s->w || y >= s->h) return;
    unsigned int byte_off = (y * s->w + x) * 4;
    unsigned int fi  = byte_off / 4096;
    unsigned int off = byte_off % 4096;
    if ((int)fi >= s->n_frames) return;
    unsigned int *p = (unsigned int *)((char *)P2V(s->frames[fi]) + off);
    *p = argb;
}

/* Fill n pixels with argb starting at window-buffer byte offset byte_off.
 * Walks page boundaries without per-pixel division. */
static void win_fill_span(struct win_slot *s,
                          unsigned int byte_off, unsigned int n,
                          unsigned int argb)
{
    unsigned int done = 0;
    while (done < n) {
        unsigned int fi  = byte_off / 4096;
        unsigned int off = byte_off % 4096;
        if ((int)fi >= s->n_frames) break;
        unsigned int *p   = (unsigned int *)((char *)P2V(s->frames[fi]) + off);
        unsigned int avail = (4096 - off) / 4;
        unsigned int cnt   = n - done;
        if (avail < cnt) cnt = avail;
        for (unsigned int i = 0; i < cnt; i++) p[i] = argb;
        done     += cnt;
        byte_off += cnt * 4;
    }
}

/* Put one pixel into the window buffer. Returns 1 if handled, 0 to fall through. */
int win_redirect_putpixel(int pid, unsigned int x, unsigned int y, unsigned int rgb)
{
    int si = slot_for_pid(pid);
    if (si < 0) return 0;
    win_write_px(&slots[si], x, y, 0xFF000000u | rgb);
    return 1;
}

/* Fill a rectangle in the window buffer. Returns 1 if handled, 0 to fall through. */
int win_redirect_fillrect(int pid,
                          unsigned int x, unsigned int y,
                          unsigned int w, unsigned int h,
                          unsigned int rgb)
{
    int si = slot_for_pid(pid);
    if (si < 0) return 0;
    struct win_slot *s = &slots[si];
    if (x >= s->w || y >= s->h || w == 0 || h == 0) return 1;
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    unsigned int argb     = 0xFF000000u | rgb;
    unsigned int row_stride = s->w * 4;
    for (unsigned int row = 0; row < h; row++) {
        win_fill_span(s, (y + row) * row_stride + x * 4, w, argb);
    }
    return 1;
}

/* Blit a user-supplied ARGB buffer into the window buffer. Returns 1/0. */
int win_redirect_blit(int pid,
                      unsigned int x, unsigned int y,
                      unsigned int w, unsigned int h,
                      const unsigned int *src)
{
    int si = slot_for_pid(pid);
    if (si < 0) return 0;
    struct win_slot *s = &slots[si];
    for (unsigned int row = 0; row < h; row++) {
        for (unsigned int col = 0; col < w; col++) {
            unsigned int px = src[row * w + col];
            if (px >> 24)   /* skip transparent pixels */
                win_write_px(s, x + col, y + row, px);
        }
    }
    return 1;
}

/* ── Compositor ──────────────────────────────────────────────────────────── */

/* Blit one window to the framebuffer. Handles page boundaries. */
static void blit_window(struct win_slot *s)
{
    unsigned int row_bytes = s->w * 4;
    for (unsigned int row = 0; row < s->h; row++) {
        unsigned int byte_off = row * row_bytes;
        unsigned int col = 0;
        while (col < s->w) {
            unsigned int fi  = byte_off / 4096;
            unsigned int off = byte_off % 4096;
            if ((int)fi >= s->n_frames) break;
            const unsigned int *src =
                (const unsigned int *)((const char *)P2V(s->frames[fi]) + off);
            unsigned int avail_px = (4096 - off) / 4;
            unsigned int need_px  = s->w - col;
            unsigned int n = (avail_px < need_px) ? avail_px : need_px;
            fb_blit_row((unsigned int)(s->x + (int)col),
                        (unsigned int)(s->y + (int)row), n, src);
            col      += n;
            byte_off += n * 4;
        }
    }
}

/* Simple insertion sort on a local index array by z-order (ascending). */
static void sort_by_z(int *order, int n)
{
    for (int i = 1; i < n; i++) {
        int key = order[i];
        int j = i - 1;
        while (j >= 0 && slots[order[j]].z > slots[key].z) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }
}

/* Route accumulated mouse delta to the focused window's event queue. */
static void route_mouse_events(void)
{
    int mx, my;
    unsigned char mb;
    mouse_get_state(&mx, &my, &mb);

    /* Find topmost visible window at mouse position. */
    int top_si = -1;
    int top_z  = -1;
    for (int i = 0; i < WIN_MAX; i++) {
        struct win_slot *s = &slots[i];
        if (!s->pid || !s->visible) continue;
        if (mx < s->x || mx >= s->x + (int)s->w) continue;
        if (my < s->y || my >= s->y + (int)s->h) continue;
        if (s->z > top_z) { top_z = s->z; top_si = i; }
    }

    if (top_si < 0) return;

    struct win_slot *s = &slots[top_si];
    struct win_event ev;
    ev.type    = WIN_EV_MOUSE;
    ev.x       = mx - s->x;
    ev.y       = my - s->y;
    ev.buttons = (int)mb;
    ev.key     = 0;
    ev_push(s, &ev);

    /* Click focuses the window */
    if (mb && focused_slot != top_si)
        focused_slot = top_si;
}

void win_composite(void)
{
    /* Collect visible window indices. */
    int order[WIN_MAX];
    int n = 0;
    for (int i = 0; i < WIN_MAX; i++)
        if (slots[i].pid && slots[i].visible)
            order[n++] = i;
    if (n == 0) return;

    /* Erase cursor before drawing (saves region under cursor). */
    mouse_undraw_cursor();

    /* Sort back-to-front and blit each window. */
    sort_by_z(order, n);
    for (int i = 0; i < n; i++) {
        struct win_slot *s = &slots[order[i]];
        blit_window(s);
        s->dirty = 0;
    }

    /* Route mouse events to the window under the cursor. */
    route_mouse_events();

    /* Redraw cursor on top. */
    mouse_draw_cursor(0xFFFFFFFF);
}

/* ── Timer tick ─────────────────────────────────────────────────────────── */

void win_tick(void)
{
    static int cnt = 0;
    /* Fire compositor every 3 ticks: PIT=100Hz → 33ms ≈ 30fps. */
    if (++cnt < 3)
        return;
    cnt = 0;

    /* Only composite if at least one window is visible. */
    for (int i = 0; i < WIN_MAX; i++) {
        if (slots[i].pid && slots[i].visible) {
            win_composite();
            return;
        }
    }
}
