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
//! ## References
//!
//! - Colour palette: mirrors StinkOS Pages dark theme (style.css).
//! - Widget state model: adapted from Makepad widgets/src/button.rs
//!   (hover/down/normal state pattern).
//! - Window decorations: adapted from ToaruOS lib/decor-fancy.c
//!   (TITLEBAR_HEIGHT=33, close button placement, active/inactive colours).
//! - Compositor z-order pattern: adapted from Orbital src/window_order.rs.

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
    fn sys_kill(pid: i32) -> i32;
    fn sys_getpid() -> i32;
    fn sys_arp_info(buf: *mut u8, cap: i32) -> i32;
}

// ── Safe drawing wrappers ────────────────────────────────────────────────────

pub fn fill(x: i32, y: i32, w: i32, h: i32, rgb: u32) {
    unsafe { sys_fillrect(x, y, w, h, rgb); }
}

/// Draw NUL-terminated text. `s` must end with a 0x00 byte.
pub fn text(x: i32, y: i32, s: &[u8], rgb: u32) {
    unsafe { sys_drawtext(x, y, s.as_ptr(), rgb); }
}

pub fn pixel(x: i32, y: i32, rgb: u32) {
    unsafe { sys_draw(x, y, rgb); }
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
    text(16, (TASKBAR_H - 8) / 2, b"StinkOS\0", ACCENT);

    let mut t = RtcTime { year: 0, month: 0, day: 0, hour: 0, minute: 0, second: 0 };
    if read_clock(&mut t) == 0 {
        let mut buf = [0u8; 6];
        fmt_hhmm(t.hour, t.minute, &mut buf);
        let tw = 5 * 8; // "HH:MM" = 5 chars × 8 px each
        text(screen_w - tw - 16, (TASKBAR_H - 8) / 2, &buf, FG_DIM);
    }
}

// ── Cursor ───────────────────────────────────────────────────────────────────

/// Draw a cross-hair cursor centred at (cx, cy).
/// Inspired by ToaruOS's 7-pixel cross cursor (mouse.c).
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
/// State model adapted from Makepad widgets/src/button.rs.
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
        let ly = self.y + (self.h - 8) / 2;
        text(lx, ly, self.label, fg);
    }

    pub fn contains(&self, px: i32, py: i32) -> bool {
        px >= self.x && px < self.x + self.w &&
        py >= self.y && py < self.y + self.h
    }
}

// ── Tile widget ──────────────────────────────────────────────────────────────

/// An app-launcher tile.
///
/// State model: normal / hovered.  (No pressed state — click is instant.)
/// Adapted from Makepad button.rs which mixes colour based on hover/down floats;
/// we use a simple bool since we have no interpolation timer.
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
        let label_y   = self.y + 14 + icon_h + 8;
        text(label_x, label_y, self.label, fg);
    }

    pub fn contains(&self, px: i32, py: i32) -> bool {
        px >= self.x && px < self.x + self.w &&
        py >= self.y && py < self.y + self.h
    }
}
