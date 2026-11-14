//! libui -- windows, drawing and widgets for StinkOS Rust programs.
//!
//! A program opens one [`Window`], draws into it through the clipped
//! primitives of [`Surface`], calls [`Window::flush`] and waits for the next
//! [`Event`]. The kernel draws the title bar, the close box and the border,
//! and tells the program about input in window coordinates.
//!
//! ```ignore
//! let Some(mut win) = Window::open(b"Hello\0", 320, 120, 0) else { quit(1) };
//! loop {
//!     win.fill(0, 0, win.w(), win.h(), BG);
//!     win.text(16, 16, b"hello\0", FG);
//!     win.flush();
//!     match win.wait(1000) {
//!         Some(Event::Close) | Some(Event::Key(KEY_ESC)) => break,
//!         _ => {}
//!     }
//! }
//! ```
//!
//! Everything that can fail says so: [`Window::open`] returns `None`
//! when the kernel could not create the window (no memory, too many
//! windows, size out of range) and the program must not draw. Drawing
//! itself cannot go out of bounds -- the primitives are implemented once,
//! in `lib/libstink_surface.c`, and clip every coordinate.
//!
//! The crate has no Rust dependencies; the C symbols come from
//! `LIBSTINK_OBJS` at link time.

#![no_std]

// ── Palette ─────────────────────────────────────────────────────────────────
//
// One dark theme for every program, matching the kernel's window chrome.

pub const BG:          u32 = 0x0d1117;   // window background
pub const SURFACE:     u32 = 0x161b22;   // raised areas: buttons, rows, panels
pub const SURFACE_ALT: u32 = 0x21262d;   // hovered / selected
pub const BORDER:      u32 = 0x30363d;
pub const FG:          u32 = 0xe6edf3;
pub const FG_DIM:      u32 = 0x8b949e;
pub const ACCENT:      u32 = 0x57f287;
pub const RED:         u32 = 0xf47067;
pub const YELLOW:      u32 = 0xfee75c;

/// Glyph cell of the UI font.
pub const FONT_W: i32 = 8;
pub const FONT_H: i32 = 16;

/// Standard inner margin and spacing, so programs line up with each other.
pub const PAD: i32 = 12;
pub const GAP: i32 = 8;
/// Standard heights.
pub const ROW_H: i32 = 20;
pub const BTN_H: i32 = 28;

// ── Screen ──────────────────────────────────────────────────────────────────

pub const SCREEN_W: i32 = 1024;
pub const SCREEN_H: i32 = 768;
pub const TASKBAR_H: i32 = 28;
/// What the kernel adds around a decorated client area.
pub const CHROME_W: i32 = 2;
pub const CHROME_H: i32 = 26;
/// Largest client area of a decorated window.
pub const MAX_W: i32 = SCREEN_W - CHROME_W;
pub const MAX_H: i32 = SCREEN_H - TASKBAR_H - CHROME_H;

pub const WIN_DESKTOP:   u32 = 0x1;
pub const WIN_RESIZABLE: u32 = 0x2;

// ── Keys ────────────────────────────────────────────────────────────────────
//
// `Event::Key` carries the same codes the kernel's character stream uses.

pub const KEY_BACKSPACE: u8 = 8;
pub const KEY_TAB:       u8 = 9;
pub const KEY_ENTER:     u8 = b'\n';
pub const KEY_ESC:       u8 = 27;
pub const KEY_UP:        u8 = 28;
pub const KEY_DOWN:      u8 = 29;
pub const KEY_LEFT:      u8 = 30;
pub const KEY_RIGHT:     u8 = 31;
pub const KEY_HOME:      u8 = 250;
pub const KEY_END:       u8 = 251;
pub const KEY_PGUP:      u8 = 252;
pub const KEY_PGDN:      u8 = 253;

/// Ctrl+letter arrives as the ASCII control code: `ctrl(b's') == 19`.
pub const fn ctrl(letter: u8) -> u8 { letter - b'a' + 1 }

/// Scancodes (set 1) of keys that have no character, for `Event::Scan`.
pub const SC_F1:     u8 = 0x3B;
pub const SC_F11:    u8 = 0x57;
pub const SC_DELETE: u8 = 0x53;   // with `extended`

// ── FFI ─────────────────────────────────────────────────────────────────────

#[repr(C)]
struct RawSurface {
    px: *mut u32,
    w:  i32,
    h:  i32,
}

#[repr(C)]
struct RawEvent {
    kind:    i32,
    x:       i32,
    y:       i32,
    buttons: i32,
    key:     i32,
}

#[repr(C)]
pub struct RtcTime {
    pub year:   u32,
    pub month:  u32,
    pub day:    u32,
    pub hour:   u32,
    pub minute: u32,
    pub second: u32,
}

extern "C" {
    // lib/libstink_surface.c
    fn surf_pixel(s: *const RawSurface, x: i32, y: i32, rgb: u32);
    fn surf_fill(s: *const RawSurface, x: i32, y: i32, w: i32, h: i32, rgb: u32);
    fn surf_rect(s: *const RawSurface, x: i32, y: i32, w: i32, h: i32, rgb: u32);
    fn surf_line(s: *const RawSurface, x0: i32, y0: i32, x1: i32, y1: i32, rgb: u32);
    fn surf_text_n(s: *const RawSurface, x: i32, y: i32, str: *const u8, n: i32, rgb: u32) -> i32;

    // window manager (lib/libstink_syms.c)
    fn stk_win_create(w: u32, h: u32, flags: u32) -> i32;
    fn stk_win_show(x: i32, y: i32, title: *const u8) -> i32;
    fn stk_win_flush();
    fn stk_win_destroy();
    fn stk_win_get_event(ev: *mut RawEvent) -> i32;
    fn stk_win_wait(timeout_ms: u32) -> i32;
    fn stk_win_raise();
    fn stk_win_move(x: i32, y: i32);
    fn stk_win_resize(w: u32, h: u32) -> i32;
    fn stk_reap(code: *mut i32) -> i32;
    fn stk_clip_write(buf: *const u8, len: u32) -> i32;
    fn stk_clip_read(buf: *mut u8, max: u32) -> i32;
    fn stk_meminfo(out3: *mut u32) -> i32;
    fn stk_netinfo(out: *mut NetInfo) -> i32;

    // the rest of the system
    fn sys_log(s: *const u8);
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
}

const WIN_BASE: usize = 0x1200_0000;

// ── Geometry ────────────────────────────────────────────────────────────────

#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Rect {
    pub x: i32,
    pub y: i32,
    pub w: i32,
    pub h: i32,
}

impl Rect {
    pub const fn new(x: i32, y: i32, w: i32, h: i32) -> Rect { Rect { x, y, w, h } }

    pub fn contains(&self, px: i32, py: i32) -> bool {
        px >= self.x && px < self.x + self.w && py >= self.y && py < self.y + self.h
    }

    /// The rectangle shrunk by `d` on every side.
    pub fn inset(&self, d: i32) -> Rect {
        Rect::new(self.x + d, self.y + d, self.w - 2 * d, self.h - 2 * d)
    }

    /// Cell `i` of `n` equal columns separated by `gap`.
    pub fn column(&self, i: i32, n: i32, gap: i32) -> Rect {
        let cw = (self.w - gap * (n - 1)) / n;
        Rect::new(self.x + i * (cw + gap), self.y, cw, self.h)
    }

    /// Cell `i` of `n` equal rows separated by `gap`.
    pub fn row(&self, i: i32, n: i32, gap: i32) -> Rect {
        let rh = (self.h - gap * (n - 1)) / n;
        Rect::new(self.x, self.y + i * (rh + gap), self.w, rh)
    }
}

// ── Surface ─────────────────────────────────────────────────────────────────

/// A rectangle of XRGB pixels. Every method clips to it.
pub struct Surface {
    raw: RawSurface,
}

impl Surface {
    pub fn w(&self) -> i32 { self.raw.w }
    pub fn h(&self) -> i32 { self.raw.h }
    pub fn bounds(&self) -> Rect { Rect::new(0, 0, self.raw.w, self.raw.h) }

    pub fn clear(&self, rgb: u32) {
        self.fill(0, 0, self.raw.w, self.raw.h, rgb);
    }

    pub fn pixel(&self, x: i32, y: i32, rgb: u32) {
        unsafe { surf_pixel(&self.raw, x, y, rgb) }
    }

    pub fn fill(&self, x: i32, y: i32, w: i32, h: i32, rgb: u32) {
        unsafe { surf_fill(&self.raw, x, y, w, h, rgb) }
    }

    pub fn fill_rect(&self, r: Rect, rgb: u32) {
        self.fill(r.x, r.y, r.w, r.h, rgb);
    }

    /// 1-pixel outline.
    pub fn rect(&self, r: Rect, rgb: u32) {
        unsafe { surf_rect(&self.raw, r.x, r.y, r.w, r.h, rgb) }
    }

    pub fn line(&self, x0: i32, y0: i32, x1: i32, y1: i32, rgb: u32) {
        unsafe { surf_line(&self.raw, x0, y0, x1, y1, rgb) }
    }

    pub fn hline(&self, x: i32, y: i32, w: i32, rgb: u32) { self.fill(x, y, w, 1, rgb); }
    pub fn vline(&self, x: i32, y: i32, h: i32, rgb: u32) { self.fill(x, y, 1, h, rgb); }

    /// Draw `s` up to its first NUL (or its end). Transparent background.
    /// Returns the width drawn in pixels.
    pub fn text(&self, x: i32, y: i32, s: &[u8], rgb: u32) -> i32 {
        let n = text_len(s) as i32;
        unsafe { surf_text_n(&self.raw, x, y, s.as_ptr(), n, rgb) }
    }

    /// Like [`text`](Self::text), but never wider than `max_w` pixels.
    pub fn text_fit(&self, x: i32, y: i32, s: &[u8], max_w: i32, rgb: u32) -> i32 {
        let mut n = text_len(s) as i32;
        let fit = if max_w > 0 { max_w / FONT_W } else { 0 };
        if n > fit { n = fit; }
        unsafe { surf_text_n(&self.raw, x, y, s.as_ptr(), n, rgb) }
    }

    /// Text centred in `r`, clipped to its width.
    pub fn text_center(&self, r: Rect, s: &[u8], rgb: u32) {
        let mut n = text_len(s) as i32;
        let fit = if r.w > 0 { r.w / FONT_W } else { 0 };
        if n > fit { n = fit; }
        let x = r.x + (r.w - n * FONT_W) / 2;
        let y = r.y + (r.h - FONT_H) / 2;
        unsafe { surf_text_n(&self.raw, x, y, s.as_ptr(), n, rgb); }
    }

    /// Text ending at `right`.
    pub fn text_right(&self, right: i32, y: i32, s: &[u8], rgb: u32) {
        let n = text_len(s) as i32;
        unsafe { surf_text_n(&self.raw, right - n * FONT_W, y, s.as_ptr(), n, rgb); }
    }
}

/// Characters before the first NUL.
pub fn text_len(s: &[u8]) -> usize {
    s.iter().position(|&b| b == 0).unwrap_or(s.len())
}

/// Width in pixels of `s` in the UI font.
pub fn text_w(s: &[u8]) -> i32 {
    text_len(s) as i32 * FONT_W
}

// ── Events ──────────────────────────────────────────────────────────────────

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Event {
    /// Pointer moved or a button changed. `x`, `y` are client coordinates
    /// (they can lie outside the window while a button is held).
    /// `buttons` is the state after the event (bit 0 left, 1 right,
    /// 2 middle); `changed` has a bit set for each button that flipped.
    Mouse { x: i32, y: i32, buttons: u8, changed: u8 },
    /// A character, or one of the `KEY_*` codes.
    Key(u8),
    /// A raw key transition, for keys that produce no character.
    Scan { code: u8, pressed: bool, extended: bool },
    /// The user clicked the close box.
    Close,
    /// The user asked for a new client size; answer with [`Window::resize`].
    Resize { w: i32, h: i32 },
    Focus(bool),
}

impl Event {
    /// Left button went down in this event: `Some((x, y))`.
    pub fn left_press(&self) -> Option<(i32, i32)> {
        match *self {
            Event::Mouse { x, y, buttons, changed } if changed & 1 != 0 && buttons & 1 != 0 => Some((x, y)),
            _ => None,
        }
    }

    /// Left button came up in this event.
    pub fn left_release(&self) -> Option<(i32, i32)> {
        match *self {
            Event::Mouse { x, y, buttons, changed } if changed & 1 != 0 && buttons & 1 == 0 => Some((x, y)),
            _ => None,
        }
    }
}

// ── Window ──────────────────────────────────────────────────────────────────

/// The program's window. Dereferences to its [`Surface`].
pub struct Window {
    surf: Surface,
}

impl core::ops::Deref for Window {
    type Target = Surface;
    fn deref(&self) -> &Surface { &self.surf }
}

impl Window {
    /// Create and show a window with a `w` x `h` client area; the kernel
    /// picks the position. `title` must be NUL-terminated.
    /// `None` when the window could not be created -- do not draw.
    pub fn open(title: &[u8], w: i32, h: i32, flags: u32) -> Option<Window> {
        Window::open_at(title, w, h, -1, -1, flags)
    }

    /// As [`open`](Self::open), with the outer top-left corner at (x, y).
    pub fn open_at(title: &[u8], w: i32, h: i32, x: i32, y: i32, flags: u32) -> Option<Window> {
        if w <= 0 || h <= 0 || title.last() != Some(&0) {
            return None;
        }
        unsafe {
            if stk_win_create(w as u32, h as u32, flags) != 0 {
                sys_log(b"libui: the kernel refused to create the window\0".as_ptr());
                return None;
            }
            if stk_win_show(x, y, title.as_ptr()) != 0 {
                stk_win_destroy();
                return None;
            }
        }
        Some(Window { surf: Surface { raw: RawSurface { px: WIN_BASE as *mut u32, w, h } } })
    }

    /// Tell the compositor the frame is ready.
    pub fn flush(&self) {
        unsafe { stk_win_flush() }
    }

    /// Next queued event, without waiting.
    pub fn poll(&mut self) -> Option<Event> {
        let mut raw = RawEvent { kind: 0, x: 0, y: 0, buttons: 0, key: 0 };
        loop {
            if unsafe { stk_win_get_event(&mut raw) } != 0 {
                return None;
            }
            let ev = match raw.kind {
                1 => Event::Mouse { x: raw.x, y: raw.y, buttons: raw.buttons as u8, changed: raw.key as u8 },
                2 => Event::Key(raw.key as u8),
                3 => Event::Close,
                4 => Event::Resize { w: raw.x, h: raw.y },
                5 => Event::Scan {
                    code:     (raw.key & 0x7F) as u8,
                    pressed:  raw.key & 0x8000 != 0,
                    extended: raw.key & 0x0100 != 0,
                },
                6 => Event::Focus(raw.x != 0),
                _ => continue,                 // a kind this library does not know: skip it
            };
            return Some(ev);
        }
    }

    /// Next event, sleeping up to `timeout_ms` for one. `None` on timeout.
    /// The process uses no CPU while it waits.
    pub fn wait(&mut self, timeout_ms: u32) -> Option<Event> {
        if let Some(ev) = self.poll() {
            return Some(ev);
        }
        unsafe { stk_win_wait(timeout_ms); }
        self.poll()
    }

    /// Change the client size (the answer to `Event::Resize`). The buffer
    /// is reallocated, so redraw everything afterwards. Returns false and
    /// keeps the old size if the kernel could not do it.
    pub fn resize(&mut self, w: i32, h: i32) -> bool {
        if w <= 0 || h <= 0 || unsafe { stk_win_resize(w as u32, h as u32) } != 0 {
            return false;
        }
        self.surf.raw.w = w;
        self.surf.raw.h = h;
        true
    }

    pub fn raise(&self) {
        unsafe { stk_win_raise() }
    }

    pub fn move_to(&self, x: i32, y: i32) {
        unsafe { stk_win_move(x, y) }
    }

    pub fn close(self) {
        unsafe { stk_win_destroy() }
    }
}

// ── Widgets ─────────────────────────────────────────────────────────────────
//
// Deliberately few: what the bundled programs share. A widget is a value
// that knows how to draw itself and where it is; the program owns the
// state.

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum BtnState { Normal, Hover, Pressed, Disabled }

/// A push button.
pub struct Button {
    pub rect:  Rect,
    pub label: &'static [u8],
}

impl Button {
    pub const fn new(rect: Rect, label: &'static [u8]) -> Button { Button { rect, label } }

    pub fn draw(&self, s: &Surface, state: BtnState) {
        let (bg, fg, edge) = match state {
            BtnState::Normal   => (SURFACE,     FG,     BORDER),
            BtnState::Hover    => (SURFACE_ALT, FG,     ACCENT),
            BtnState::Pressed  => (ACCENT,      BG,     ACCENT),
            BtnState::Disabled => (BG,          BORDER, BORDER),
        };
        s.fill_rect(self.rect, bg);
        s.rect(self.rect, edge);
        s.text_center(self.rect, self.label, fg);
    }

    pub fn hit(&self, x: i32, y: i32) -> bool { self.rect.contains(x, y) }

    /// State for a pointer at (x, y) with the left button up or down.
    pub fn state_for(&self, x: i32, y: i32, left_down: bool) -> BtnState {
        if !self.hit(x, y) { BtnState::Normal }
        else if left_down { BtnState::Pressed }
        else { BtnState::Hover }
    }
}

/// A one-line header: dim caption text with a rule under it. Returns the y
/// coordinate below it.
pub fn section(s: &Surface, x: i32, y: i32, w: i32, caption: &[u8]) -> i32 {
    s.text(x, y, caption, FG_DIM);
    s.hline(x, y + FONT_H + 3, w, BORDER);
    y + FONT_H + 3 + GAP
}

/// "label ........ value" on one row; the value is right-aligned at `right`.
pub fn field(s: &Surface, x: i32, y: i32, right: i32, label: &[u8], value: &[u8], value_rgb: u32) {
    s.text(x, y, label, FG_DIM);
    s.text_right(right, y, value, value_rgb);
}

/// A status line along the bottom edge of the window (key hints).
pub fn status_bar(s: &Surface, text: &[u8]) {
    let y = s.h() - ROW_H - 4;
    s.fill(0, y, s.w(), ROW_H + 4, SURFACE);
    s.hline(0, y, s.w(), BORDER);
    s.text_fit(PAD, y + 4, text, s.w() - 2 * PAD, FG_DIM);
}

/// Height [`status_bar`] takes from the bottom of the window.
pub const STATUS_H: i32 = ROW_H + 4;

/// One row of a list: highlighted when selected.
pub fn list_row(s: &Surface, r: Rect, text: &[u8], selected: bool) {
    if selected {
        s.fill_rect(r, SURFACE_ALT);
        s.fill(r.x, r.y, 3, r.h, ACCENT);
    }
    s.text_fit(r.x + 10, r.y + (r.h - FONT_H) / 2, text, r.w - 14, if selected { FG } else { FG_DIM });
}

// ── System ──────────────────────────────────────────────────────────────────

pub fn log(s: &[u8]) {
    if s.last() == Some(&0) {
        unsafe { sys_log(s.as_ptr()) }
    }
}

pub fn ticks() -> u32 {
    unsafe { sys_ticks() }
}

pub fn sleep_ms(ms: u32) {
    unsafe { sys_sleep_ms(ms) }
}

pub fn quit(code: i32) -> ! {
    unsafe { sys_exit_code(code) }
}

pub fn read_clock(out: &mut RtcTime) -> bool {
    unsafe { sys_rtc_read(out as *mut RtcTime) == 0 }
}

pub fn fcount() -> i32 {
    unsafe { sys_fcount() }
}

/// Name of file `index` into `name`; returns its size, or -1.
pub fn finfo(index: i32, name: &mut [u8; 16]) -> i32 {
    unsafe { sys_finfo(index, name.as_mut_ptr()) }
}

/// Read a file. `name` must be NUL-terminated. Returns bytes read or -1.
pub fn fread(name: &[u8], buf: &mut [u8]) -> i32 {
    if name.last() != Some(&0) { return -1; }
    unsafe { sys_fread(name.as_ptr(), buf.as_mut_ptr(), buf.len() as u32) }
}

/// Create or replace a file. Returns true on success.
pub fn fwrite(name: &[u8], buf: &[u8]) -> bool {
    if name.last() != Some(&0) { return false; }
    unsafe { sys_fwrite(name.as_ptr(), buf.as_ptr(), buf.len() as u32) == 0 }
}

pub fn fdelete(name: &[u8]) -> bool {
    if name.last() != Some(&0) { return false; }
    unsafe { sys_fdelete(name.as_ptr()) == 0 }
}

/// Physical memory, in 4 KiB pages.
#[derive(Clone, Copy)]
pub struct MemInfo {
    pub total_pages: u32,
    pub free_pages:  u32,
}

pub fn meminfo() -> Option<MemInfo> {
    let mut raw = [0u32; 3];
    if unsafe { stk_meminfo(raw.as_mut_ptr()) } != 0 {
        return None;
    }
    Some(MemInfo { total_pages: raw[0], free_pages: raw[1] })
}

/// Network configuration as the kernel has it. Addresses are in network
/// byte order: the first octet is the low byte.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct NetInfo {
    pub ip:         u32,
    pub mask:       u32,
    pub gateway:    u32,
    pub dns:        u32,
    pub mac:        [u8; 6],
    /// 0 init, 1 discovering, 2 requesting, 3 bound, 4 failed.
    pub dhcp_state: u8,
    pub link_up:    u8,
}

pub fn netinfo() -> Option<NetInfo> {
    let mut ni = NetInfo { ip: 0, mask: 0, gateway: 0, dns: 0, mac: [0; 6], dhcp_state: 0, link_up: 0 };
    if unsafe { stk_netinfo(&mut ni) } != 0 {
        return None;
    }
    Some(ni)
}

/// "a.b.c.d\0" for an address in network byte order. Returns the length.
pub fn fmt_ip(ip: u32, buf: &mut [u8; 16]) -> usize {
    let mut p = 0;
    for i in 0..4 {
        if i > 0 { p = push_str(buf, p, b".\0"); }
        p = push_u32(buf, p, (ip >> (8 * i)) & 0xFF);
    }
    p
}

/// "aa:bb:cc:dd:ee:ff\0"
pub fn fmt_mac(mac: &[u8; 6], buf: &mut [u8; 18]) {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    for i in 0..6 {
        buf[i * 3]     = HEX[(mac[i] >> 4) as usize];
        buf[i * 3 + 1] = HEX[(mac[i] & 15) as usize];
        buf[i * 3 + 2] = if i < 5 { b':' } else { 0 };
    }
}

/// Process table as text (one row per process). Returns bytes written.
pub fn proc_info(buf: &mut [u8]) -> usize {
    let n = unsafe { sys_proc_info(buf.as_mut_ptr(), buf.len() as i32) };
    if n < 0 { 0 } else { n as usize }
}

pub fn arp_info(buf: &mut [u8]) -> usize {
    let n = unsafe { sys_arp_info(buf.as_mut_ptr(), buf.len() as i32) };
    if n < 0 { 0 } else { n as usize }
}

pub fn kill(pid: i32) -> bool {
    unsafe { sys_kill(pid) == 0 }
}

pub fn getpid() -> i32 {
    unsafe { sys_getpid() }
}

/// 0 = US, 1 = BR. Returns the previous layout.
pub fn set_keymap(layout: i32) -> i32 {
    unsafe { sys_set_keymap(layout) }
}

/// PC speaker tone in Hz; 0 turns it off.
pub fn sound(freq: u32) {
    unsafe { sys_sound(freq) }
}

/// Copy `data` to the system clipboard. Returns bytes stored.
pub fn clip_write(data: &[u8]) -> usize {
    let n = unsafe { stk_clip_write(data.as_ptr(), data.len() as u32) };
    if n < 0 { 0 } else { n as usize }
}

pub fn clip_read(buf: &mut [u8]) -> usize {
    let n = unsafe { stk_clip_read(buf.as_mut_ptr(), buf.len() as u32) };
    if n < 0 { 0 } else { n as usize }
}

/// Start `name` (NUL-terminated, e.g. `b"rs-calc\0"`) as a separate
/// process. Returns its pid, or -1. The caller keeps running; collect the
/// child later with [`reap`].
pub fn spawn(name: &[u8]) -> i32 {
    if name.last() != Some(&0) { return -1; }
    let pid = unsafe { sys_fork() };
    if pid == 0 {
        unsafe {
            sys_exec(name.as_ptr());
            sys_exit_code(127);                // exec failed: this copy must not go on as a second parent
        }
    }
    pid
}

/// Collect one exited child without waiting. `Some((pid, exit_code))`.
pub fn reap() -> Option<(i32, i32)> {
    let mut code = 0i32;
    let pid = unsafe { stk_reap(&mut code) };
    if pid > 0 { Some((pid, code)) } else { None }
}

/// Wait for child `pid` to exit; returns its exit code.
pub fn wait_for(pid: i32) -> i32 {
    unsafe { sys_waitpid(pid) }
}

// ── Formatting ──────────────────────────────────────────────────────────────

fn digit2(n: u32, buf: &mut [u8], pos: usize) {
    buf[pos]     = b'0' + ((n / 10) % 10) as u8;
    buf[pos + 1] = b'0' + (n % 10) as u8;
}

/// "HH:MM\0"
pub fn fmt_hhmm(hour: u32, min: u32, buf: &mut [u8; 6]) {
    digit2(hour, buf, 0);
    buf[2] = b':';
    digit2(min, buf, 3);
    buf[5] = 0;
}

/// "HH:MM:SS\0"
pub fn fmt_hhmmss(hour: u32, min: u32, sec: u32, buf: &mut [u8; 9]) {
    digit2(hour, buf, 0);
    buf[2] = b':';
    digit2(min, buf, 3);
    buf[5] = b':';
    digit2(sec, buf, 6);
    buf[8] = 0;
}

/// Decimal of `n`, NUL-terminated. Returns the digit count.
pub fn fmt_u32(mut n: u32, buf: &mut [u8; 12]) -> usize {
    if n == 0 {
        buf[0] = b'0';
        buf[1] = 0;
        return 1;
    }
    let mut tmp = [0u8; 10];
    let mut len = 0usize;
    while n > 0 {
        tmp[len] = b'0' + (n % 10) as u8;
        n /= 10;
        len += 1;
    }
    for i in 0..len {
        buf[i] = tmp[len - 1 - i];
    }
    buf[len] = 0;
    len
}

/// Append `src` (up to its NUL) to `dst` at `pos`, keeping `dst`
/// NUL-terminated. Returns the new length. Never overruns `dst`.
pub fn push_str(dst: &mut [u8], pos: usize, src: &[u8]) -> usize {
    let mut p = pos;
    for &b in src {
        if b == 0 || p + 1 >= dst.len() { break; }
        dst[p] = b;
        p += 1;
    }
    if p < dst.len() { dst[p] = 0; }
    p
}

/// Append the decimal of `n`.
pub fn push_u32(dst: &mut [u8], pos: usize, n: u32) -> usize {
    let mut tmp = [0u8; 12];
    fmt_u32(n, &mut tmp);
    push_str(dst, pos, &tmp)
}

pub fn clamp(x: i32, lo: i32, hi: i32) -> i32 {
    if x < lo { lo } else if x > hi { hi } else { x }
}
