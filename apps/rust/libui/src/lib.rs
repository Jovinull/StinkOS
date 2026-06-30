//! libui — graphical widget toolkit for StinkOS userland.
//!
//! Wraps the C drawing primitives from libstink_gfx into safe Rust, and
//! provides a small widget layer (Tile, taskbar, cursor) sufficient for a
//! full graphical desktop launcher.
//!
//! All C symbols are provided at final link time from LIBSTINK_OBJS; this
//! crate carries no Rust dependencies.
//!
//! ## Palette
//!
//! The colour constants mirror the StinkOS GitHub Pages dark theme so the
//! OS shell looks coherent with the project website.
//!

#![no_std]

// ── Palette ─────────────────────────────────────────────────────────────────

pub const BG:          u32 = 0x0d1117;
pub const SURFACE:     u32 = 0x161b22;
pub const SURFACE_ALT: u32 = 0x21262d;   // hovered surface
pub const BORDER:      u32 = 0x30363d;
pub const FG:          u32 = 0xe6edf3;
pub const FG_DIM:      u32 = 0x8b949e;
pub const ACCENT:      u32 = 0x57f287;
pub const RED:         u32 = 0xf47067;
pub const YELLOW:      u32 = 0xfee75c;
pub const SHADOW:      u32 = 0x050810;

// ── Key codes ────────────────────────────────────────────────────────────────

pub const KEY_UP:    i32 = 28;
pub const KEY_DOWN:  i32 = 29;
pub const KEY_LEFT:  i32 = 30;
pub const KEY_RIGHT: i32 = 31;

// ── RTC time ─────────────────────────────────────────────────────────────────

#[repr(C)]
pub struct RtcTime {
    pub year:   u32,
    pub month:  u32,
    pub day:    u32,
    pub hour:   u32,
    pub minute: u32,
    pub second: u32,
}

// ── FFI ─────────────────────────────────────────────────────────────────────

extern "C" {
    fn sys_fillrect(x: i32, y: i32, w: i32, h: i32, rgb: u32);
    fn sys_drawtext(x: i32, y: i32, s: *const u8, rgb: u32) -> i32;
    fn sys_draw(x: i32, y: i32, rgb: u32);
    fn draw_rect_outline(x: i32, y: i32, w: i32, h: i32, rgb: u32);
    fn draw_rounded_rect(x: i32, y: i32, w: i32, h: i32, r: i32, rgb: u32);
    fn draw_shadow(x: i32, y: i32, w: i32, h: i32, depth: i32, rgb: u32);
    fn draw_window_frame(x: i32, y: i32, w: i32, h: i32, title: *const u8) -> i32;
    fn sys_get_mouse(dx: *mut i32, dy: *mut i32, buttons: *mut i32) -> i32;
    fn sys_getkey() -> i32;
    fn sys_ticks() -> u32;
    fn sys_exec(name: *const u8) -> i32;
    fn sys_fork() -> i32;
    fn sys_waitpid(pid: i32) -> i32;
    fn sys_sleep_ms(ms: u32);
    fn sys_exit_code(code: i32) -> !;
    fn sys_rtc_read(out: *mut RtcTime) -> i32;
    fn sys_fcount() -> i32;
    fn sys_finfo(index: i32, name: *mut u8) -> i32;
    fn sys_proc_info(buf: *mut u8, cap: i32) -> i32;
    fn sys_fread(name: *const u8, buf: *mut u8, max: u32) -> i32;
    fn sys_fwrite(name: *const u8, buf: *const u8, size: u32) -> i32;
    fn sys_fdelete(name: *const u8) -> i32;
    fn sys_kill(pid: i32) -> i32;
    fn sys_getpid() -> i32;
    fn sys_arp_info(buf: *mut u8, cap: i32) -> i32;
    fn sys_set_keymap(layout: i32) -> i32;
    fn sys_sound(freq: u32);
    fn sys_drawline(x0: i32, y0: i32, x1: i32, y1: i32, rgb: u32);
    fn stink_font_str(x: i32, y: i32, s: *const u8, rgb: u32) -> i32;
    // window buffer drawing (libstink_winbuf)
    fn win_fillrect_buf(buf: *mut u32, stride: u32, x: i32, y: i32, w: i32, h: i32, rgb: u32);
    fn win_drawline_buf(buf: *mut u32, stride: u32, sw: u32, sh: u32,
                        x0: i32, y0: i32, x1: i32, y1: i32, rgb: u32);
    fn win_font_str_buf(buf: *mut u32, stride: u32,
                        x: i32, y: i32, s: *const u8, rgb: u32) -> i32;
    // window syscalls
    fn sys_win_create(w: u32, h: u32) -> i32;
    fn sys_win_show(x: i32, y: i32, title: *const u8) -> i32;
    fn sys_win_flush();
    fn sys_win_destroy();
    fn sys_win_get_event(ev: *mut WinEvent) -> i32;
    fn sys_win_raise();
    fn sys_win_move(x: i32, y: i32);
    // clipboard
    fn sys_clip_write(buf: *const u8, len: u32) -> i32;
    fn sys_clip_read(buf: *mut u8, max: u32) -> i32;
}

// ── Window event ─────────────────────────────────────────────────────────────

#[repr(C)]
pub struct WinEvent {
    pub kind:    i32,   /* 0=none 1=mouse 2=key 3=close */
    pub x:       i32,
    pub y:       i32,
    pub buttons: i32,
    pub key:     i32,
}

pub const WIN_EV_NONE:  i32 = 0;
pub const WIN_EV_MOUSE: i32 = 1;
pub const WIN_EV_KEY:   i32 = 2;
pub const WIN_EV_CLOSE: i32 = 3;

// ── Window buffer ─────────────────────────────────────────────────────────────

pub const WIN_BASE: u32  = 0x12000000;
pub const SCREEN_W: i32  = 1024;
pub const SCREEN_H: i32  = 768;

pub fn win_init(title: &[u8]) {
    unsafe {
        sys_win_create(SCREEN_W as u32, SCREEN_H as u32);
        sys_win_show(0, 0, title.as_ptr());
    }
}

pub fn win_done() {
    unsafe { sys_win_destroy(); }
}

pub fn win_flush() {
    unsafe { sys_win_flush(); }
}

pub fn win_poll_event() -> Option<WinEvent> {
    let mut ev = WinEvent { kind: 0, x: 0, y: 0, buttons: 0, key: 0 };
    let r = unsafe { sys_win_get_event(&mut ev) };
    if r == 0 { Some(ev) } else { None }
}

pub fn win_raise() {
    unsafe { sys_win_raise(); }
}

/// Return a raw mutable pointer to the window pixel buffer.
#[inline(always)]
pub fn win_buf() -> *mut u32 {
    WIN_BASE as *mut u32
}

/// Fill a rectangle in the window buffer.
pub fn wfill(x: i32, y: i32, w: i32, h: i32, rgb: u32) {
    unsafe { win_fillrect_buf(win_buf(), SCREEN_W as u32, x, y, w, h, rgb); }
}

/// Draw a line in the window buffer.
pub fn wline(x0: i32, y0: i32, x1: i32, y1: i32, rgb: u32) {
    unsafe { win_drawline_buf(win_buf(), SCREEN_W as u32,
                              SCREEN_W as u32, SCREEN_H as u32,
                              x0, y0, x1, y1, rgb); }
}

/// Set a single pixel in the window buffer.
pub fn wpixel(x: i32, y: i32, rgb: u32) {
    if x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H { return; }
    unsafe {
        *win_buf().add((y * SCREEN_W + x) as usize) = 0xFF000000 | rgb;
    }
}

/// Render NUL-terminated text into window buffer using 8×16 font.
pub fn wtext(x: i32, y: i32, s: &[u8], rgb: u32) -> i32 {
    unsafe { win_font_str_buf(win_buf(), SCREEN_W as u32, x, y, s.as_ptr(), rgb) }
}

/// Draw a 1px border rectangle in the window buffer.
pub fn wrect_outline(x: i32, y: i32, w: i32, h: i32, rgb: u32) {
    wfill(x,         y,         w, 1, rgb);
    wfill(x,         y + h - 1, w, 1, rgb);
    wfill(x,         y,         1, h, rgb);
    wfill(x + w - 1, y,         1, h, rgb);
}

// ── Safe drawing wrappers ────────────────────────────────────────────────────

pub fn fill(x: i32, y: i32, w: i32, h: i32, rgb: u32) {
    unsafe { sys_fillrect(x, y, w, h, rgb); }
}

/// Draw NUL-terminated text via 8×16 bitmap font. `s` must end with 0x00.
/// Returns pixel width consumed. Transparent background — fill BG first.
pub fn text16(x: i32, y: i32, s: &[u8], rgb: u32) -> i32 {
    unsafe { stink_font_str(x, y, s.as_ptr(), rgb) }
}

/// Draw NUL-terminated text. `s` must end with a 0x00 byte.
pub fn text(x: i32, y: i32, s: &[u8], rgb: u32) {
    unsafe { sys_drawtext(x, y, s.as_ptr(), rgb); }
}

pub fn pixel(x: i32, y: i32, rgb: u32) {
    unsafe { sys_draw(x, y, rgb); }
}

pub fn rect_outline(x: i32, y: i32, w: i32, h: i32, rgb: u32) {
    unsafe { draw_rect_outline(x, y, w, h, rgb); }
}

pub fn rounded(x: i32, y: i32, w: i32, h: i32, r: i32, rgb: u32) {
    unsafe { draw_rounded_rect(x, y, w, h, r, rgb); }
}

pub fn shadow(x: i32, y: i32, w: i32, h: i32, depth: i32, rgb: u32) {
    unsafe { draw_shadow(x, y, w, h, depth, rgb); }
}

/// Draw a window frame; returns the inner content Y start.
pub fn window_frame(x: i32, y: i32, w: i32, h: i32, title: &[u8]) -> i32 {
    unsafe { draw_window_frame(x, y, w, h, title.as_ptr()) }
}

// ── Input wrappers ───────────────────────────────────────────────────────────

/// Returns (dx, dy, buttons) from the kernel mouse accumulator.
/// dx/dy are signed screen-space deltas since last call.
/// buttons: bit 0 = left, bit 1 = right, bit 2 = middle.
pub fn poll_mouse() -> (i32, i32, i32) {
    let (mut dx, mut dy, mut btn) = (0i32, 0i32, 0i32);
    unsafe { sys_get_mouse(&mut dx, &mut dy, &mut btn); }
    (dx, dy, btn)
}

pub fn poll_key() -> i32 {
    unsafe { sys_getkey() }
}

pub fn ticks() -> u32 {
    unsafe { sys_ticks() }
}

pub fn read_clock(out: &mut RtcTime) -> i32 {
    unsafe { sys_rtc_read(out as *mut RtcTime) }
}

pub fn fcount() -> i32 {
    unsafe { sys_fcount() }
}

/// Fill name[0..16] with filename; returns file size or -1.
pub fn finfo(index: i32, name: &mut [u8; 16]) -> i32 {
    unsafe { sys_finfo(index, name.as_mut_ptr()) }
}

/// Returns bytes written into buf.
pub fn proc_info(buf: &mut [u8]) -> i32 {
    unsafe { sys_proc_info(buf.as_mut_ptr(), buf.len() as i32) }
}

/// Read a StinkFS file into buf. Returns bytes read or -1.
pub fn fread(name: &[u8], buf: &mut [u8]) -> i32 {
    unsafe { sys_fread(name.as_ptr(), buf.as_mut_ptr(), buf.len() as u32) }
}

/// Write buf to a StinkFS file. Returns bytes written or -1.
pub fn fwrite(name: &[u8], buf: &[u8]) -> i32 {
    unsafe { sys_fwrite(name.as_ptr(), buf.as_ptr(), buf.len() as u32) }
}

/// Delete a StinkFS file. Returns 0 on success, -1 if not found.
pub fn fdelete(name: &[u8]) -> i32 {
    unsafe { sys_fdelete(name.as_ptr()) }
}

/// Write data to the kernel clipboard. Returns bytes stored or -1.
pub fn clip_write(data: &[u8]) -> i32 {
    unsafe { sys_clip_write(data.as_ptr(), data.len() as u32) }
}

/// Read up to buf.len() bytes from the kernel clipboard. Returns bytes copied.
pub fn clip_read(buf: &mut [u8]) -> i32 {
    unsafe { sys_clip_read(buf.as_mut_ptr(), buf.len() as u32) }
}

/// Set keyboard layout: 0=US, 1=BR. Returns previous layout.
pub fn draw_line(x0: i32, y0: i32, x1: i32, y1: i32, rgb: u32) {
    unsafe { sys_drawline(x0, y0, x1, y1, rgb); }
}

pub fn set_keymap(layout: i32) -> i32 {
    unsafe { sys_set_keymap(layout) }
}

pub fn sound(freq: u32) {
    unsafe { sys_sound(freq); }
}

pub fn arp_info(buf: &mut [u8]) -> i32 {
    unsafe { sys_arp_info(buf.as_mut_ptr(), buf.len() as i32) }
}

pub fn kill(pid: i32) -> i32 {
    unsafe { sys_kill(pid) }
}

pub fn getpid() -> i32 {
    unsafe { sys_getpid() }
}

// ── Number formatting ────────────────────────────────────────────────────────

fn digit2(n: u32, buf: &mut [u8], pos: usize) {
    buf[pos]     = b'0' + ((n / 10) % 10) as u8;
    buf[pos + 1] = b'0' + (n % 10) as u8;
}

/// "HH:MM\0" into a 6-byte buffer.
pub fn fmt_hhmm(hour: u32, min: u32, buf: &mut [u8; 6]) {
    digit2(hour, buf, 0);
    buf[2] = b':';
    digit2(min, buf, 3);
    buf[5] = 0;
}

/// "HH:MM:SS\0" into a 9-byte buffer.
pub fn fmt_hhmmss(hour: u32, min: u32, sec: u32, buf: &mut [u8; 9]) {
    digit2(hour, buf, 0);
    buf[2] = b':';
    digit2(min, buf, 3);
    buf[5] = b':';
    digit2(sec, buf, 6);
    buf[8] = 0;
}

/// Write decimal of n into buf; NUL-terminates. Returns char count.
pub fn fmt_u32(mut n: u32, buf: &mut [u8; 12]) -> usize {
    if n == 0 {
        buf[0] = b'0'; buf[1] = 0;
        return 1;
    }
    let mut tmp = [0u8; 10];
    let mut len = 0usize;
    while n > 0 && len < 10 {
        tmp[len] = b'0' + (n % 10) as u8;
        n /= 10;
        len += 1;
    }
    for i in 0..len { buf[i] = tmp[len - 1 - i]; }
    buf[len] = 0;
    len
}

// ── Process helpers ──────────────────────────────────────────────────────────

/// Fork; returns child PID in parent, 0 in child, -1 on error.
pub fn fork() -> i32 {
    unsafe { sys_fork() }
}

/// Replace calling process image with the named app (e.g. b"snake\0").
pub fn exec(name: &[u8]) -> i32 {
    unsafe { sys_exec(name.as_ptr()) }
}

/// Block until the given child exits. Returns its exit code.
pub fn wait_for(pid: i32) -> i32 {
    unsafe { sys_waitpid(pid) }
}

pub fn sleep_ms(ms: u32) {
    unsafe { sys_sleep_ms(ms); }
}

pub fn quit(code: i32) -> ! {
    unsafe { sys_exit_code(code) }
}

// ── Helpers ──────────────────────────────────────────────────────────────────

/// Length of a NUL-terminated byte slice (up to first 0x00).
pub fn nul_len(s: &[u8]) -> usize {
    s.iter().position(|&b| b == 0).unwrap_or(s.len())
}

pub fn clamp(x: i32, lo: i32, hi: i32) -> i32 {
    if x < lo { lo } else if x > hi { hi } else { x }
}

// ── Taskbar ──────────────────────────────────────────────────────────────────

pub const TASKBAR_H:  i32 = 40;
pub const TASKBAR_BG: u32 = 0x090e14;

/// Draw the top taskbar strip with live clock on the right.
pub fn draw_taskbar(screen_w: i32) {
    fill(0, 0, screen_w, TASKBAR_H, TASKBAR_BG);
    fill(0, TASKBAR_H - 1, screen_w, 1, BORDER);
    text16(16, (TASKBAR_H - 16) / 2, b"StinkOS\0", ACCENT);

    let mut t = RtcTime { year: 0, month: 0, day: 0, hour: 0, minute: 0, second: 0 };
    if read_clock(&mut t) == 0 {
        let mut buf = [0u8; 6];
        fmt_hhmm(t.hour, t.minute, &mut buf);
        let tw = 5 * 8; // "HH:MM" = 5 chars × 8 px each
        text16(screen_w - tw - 16, (TASKBAR_H - 16) / 2, &buf, FG_DIM);
    }
}

// ── Cursor ───────────────────────────────────────────────────────────────────

/// Draw a cross-hair cursor centred at (cx, cy).
pub fn draw_cursor(cx: i32, cy: i32) {
    const ARM: i32 = 6;
    /* Shadow (1px offset) */
    fill(cx - ARM + 1, cy + 1,     ARM * 2 + 1, 1, 0x000000);
    fill(cx + 1,       cy - ARM + 1, 1, ARM * 2 + 1, 0x000000);
    /* White cross */
    fill(cx - ARM, cy,     ARM * 2 + 1, 1, 0xffffff);
    fill(cx,       cy - ARM, 1, ARM * 2 + 1, 0xffffff);
}

// ── Button widget ────────────────────────────────────────────────────────────

/// Interactive button with normal / hovered / pressed states.
#[derive(PartialEq, Eq, Clone, Copy)]
pub enum BtnState { Normal, Hovered, Pressed }

pub struct Button {
    pub label: &'static [u8],
    pub x: i32, pub y: i32,
    pub w: i32, pub h: i32,
}

impl Button {
    pub fn draw(&self, state: BtnState) {
        let (bg, fg, bdr) = match state {
            BtnState::Normal  => (SURFACE,     FG,     BORDER),
            BtnState::Hovered => (SURFACE_ALT, FG,     ACCENT),
            BtnState::Pressed => (ACCENT,      BG,     ACCENT),
        };
        shadow(self.x, self.y, self.w, self.h, 3, SHADOW);
        rounded(self.x, self.y, self.w, self.h, 6, bg);
        fill(self.x,              self.y,              self.w, 1, bdr);
        fill(self.x,              self.y + self.h - 1, self.w, 1, bdr);
        fill(self.x,              self.y,              1, self.h, bdr);
        fill(self.x + self.w - 1, self.y,              1, self.h, bdr);
        let lw = nul_len(self.label) as i32 * 8;
        let lx = self.x + (self.w - lw) / 2;
        let ly = self.y + (self.h - 16) / 2;
        text16(lx, ly, self.label, fg);
    }

    pub fn contains(&self, px: i32, py: i32) -> bool {
        px >= self.x && px < self.x + self.w &&
        py >= self.y && py < self.y + self.h
    }
}

// ── Tile widget ──────────────────────────────────────────────────────────────

/// An app-launcher tile. Normal / hovered states; click is instant.
pub struct Tile {
    /// NUL-terminated display label, e.g. `b"Doom\0"`.
    pub label: &'static [u8],
    /// NUL-terminated exec name passed to sys_exec, e.g. `b"doom1\0"`.
    pub exec:  &'static [u8],
    pub x: i32, pub y: i32,
    pub w: i32, pub h: i32,
}

impl Tile {
    pub fn draw(&self, hovered: bool) {
        let bg  = if hovered { SURFACE_ALT } else { SURFACE };
        let fg  = if hovered { ACCENT      } else { FG      };
        let bdr = if hovered { ACCENT      } else { BORDER  };

        /* Shadow first, then tile */
        shadow(self.x, self.y, self.w, self.h, 4, SHADOW);
        rounded(self.x, self.y, self.w, self.h, 8, bg);

        /* 1px border (straight lines, no rounded corners) */
        fill(self.x,              self.y,              self.w, 1, bdr);
        fill(self.x,              self.y + self.h - 1, self.w, 1, bdr);
        fill(self.x,              self.y,              1, self.h, bdr);
        fill(self.x + self.w - 1, self.y,              1, self.h, bdr);

        /* Icon placeholder: small rounded square */
        let icon_w  = 32;
        let icon_h  = 22;
        let icon_bg = if hovered { 0x2d4a38 } else { 0x1a2030 };
        let icon_x  = self.x + self.w / 2 - icon_w / 2;
        let icon_y  = self.y + 14;
        rounded(icon_x, icon_y, icon_w, icon_h, 4, icon_bg);

        /* Label centred below icon */
        let label_len = nul_len(self.label) as i32;
        let label_x   = self.x + self.w / 2 - (label_len * 8) / 2;
        let label_y   = self.y + 14 + icon_h + 6;
        text16(label_x, label_y, self.label, fg);
    }

    pub fn contains(&self, px: i32, py: i32) -> bool {
        px >= self.x && px < self.x + self.w &&
        py >= self.y && py < self.y + self.h
    }
}

// ── Widget layout ─────────────────────────────────────────────────────────────

/// A button item in a layout list. `id` is returned by hit_test on click.
/// id=255 (u8::MAX) is reserved as the "nothing" sentinel — do not use it.
pub struct UiBtn {
    pub id:    u8,
    pub label: &'static [u8],
    pub w:     i32,
    pub h:     i32,
}

/// Vertical stack of buttons, auto-positioned top-to-bottom.
pub struct UiColumn<'a> {
    pub items: &'a [UiBtn],
    pub gap:   i32,
}

impl<'a> UiColumn<'a> {
    /// Draw all buttons at (x, y), stacking downward.
    /// Pass `hov=u8::MAX` / `pressed=u8::MAX` for "nothing hovered/pressed".
    pub fn render(&self, x: i32, mut y: i32, hov: u8, pressed: u8) {
        for item in self.items {
            let state = if pressed != u8::MAX && item.id == pressed { BtnState::Pressed }
                        else if hov != u8::MAX && item.id == hov    { BtnState::Hovered }
                        else { BtnState::Normal };
            Button { label: item.label, x, y, w: item.w, h: item.h }.draw(state);
            y += item.h + self.gap;
        }
    }

    /// Return the id of the button hit at (mx, my), or None.
    pub fn hit_test(&self, x: i32, mut y: i32, mx: i32, my: i32) -> Option<u8> {
        for item in self.items {
            if mx >= x && mx < x + item.w && my >= y && my < y + item.h {
                return Some(item.id);
            }
            y += item.h + self.gap;
        }
        None
    }

    /// Total pixel height occupied by this column.
    pub fn total_height(&self) -> i32 {
        let sum: i32 = self.items.iter().map(|b| b.h).sum();
        sum + self.gap * self.items.len().saturating_sub(1) as i32
    }
}

/// Horizontal row of buttons, auto-positioned left-to-right.
pub struct UiRow<'a> {
    pub items: &'a [UiBtn],
    pub gap:   i32,
}

impl<'a> UiRow<'a> {
    /// Draw all buttons at (x, y), advancing rightward.
    /// Pass `hov=u8::MAX` / `pressed=u8::MAX` for "nothing hovered/pressed".
    pub fn render(&self, mut x: i32, y: i32, hov: u8, pressed: u8) {
        for item in self.items {
            let state = if pressed != u8::MAX && item.id == pressed { BtnState::Pressed }
                        else if hov != u8::MAX && item.id == hov    { BtnState::Hovered }
                        else { BtnState::Normal };
            Button { label: item.label, x, y, w: item.w, h: item.h }.draw(state);
            x += item.w + self.gap;
        }
    }

    /// Return the id of the button hit at (mx, my), or None.
    pub fn hit_test(&self, mut x: i32, y: i32, mx: i32, my: i32) -> Option<u8> {
        for item in self.items {
            if mx >= x && mx < x + item.w && my >= y && my < y + item.h {
                return Some(item.id);
            }
            x += item.w + self.gap;
        }
        None
    }

    /// Total pixel width occupied by this row.
    pub fn total_width(&self) -> i32 {
        let sum: i32 = self.items.iter().map(|b| b.w).sum();
        sum + self.gap * self.items.len().saturating_sub(1) as i32
    }
}
