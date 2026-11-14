//! rs-clock: seven-segment digital clock for StinkOS.
//!
//! Shows HH:MM:SS from the real-time clock, with the date under it. The
//! program sleeps between seconds: it wakes just before the RTC is due to
//! roll over, repaints when it has, and goes back to sleep.
//! Esc, Q or the close box end it.

#![no_std]
#![no_main]

use libstink::println;
use libui::*;

// ── Seven-segment geometry ──────────────────────────────────────────────────

const DIGIT_W:   i32 = 50;
const DIGIT_H:   i32 = 90;
const SEG_T:     i32 = 6;    // segment thickness
const DIGIT_GAP: i32 = 8;    // between neighbouring digits and colons
const COLON_W:   i32 = 16;

// D D : D D : D D, with DIGIT_GAP between every two neighbours.
const CLOCK_W: i32 = 6 * DIGIT_W + 2 * COLON_W + 7 * DIGIT_GAP;

// ── Window layout ───────────────────────────────────────────────────────────

/// The raised panel the digits sit in.
const PANEL: Rect = Rect::new(PAD, PAD, CLOCK_W + 2 * PAD, DIGIT_H + 2 * PAD);
const CLOCK_X: i32 = PANEL.x + PAD;
const CLOCK_Y: i32 = PANEL.y + PAD;
const DATE_Y:  i32 = PANEL.y + PANEL.h + GAP;

const WIN_W: i32 = PANEL.w + 2 * PAD;
const WIN_H: i32 = DATE_Y + FONT_H + GAP + STATUS_H;

// ── Segments ────────────────────────────────────────────────────────────────
// bit0 top, bit1 top-left, bit2 top-right, bit3 middle,
// bit4 bottom-left, bit5 bottom-right, bit6 bottom

const SEGS: [u8; 10] = [
    0x77, // 0: all but the middle
    0x24, // 1: top-right, bottom-right
    0x5D, // 2: top, top-right, middle, bottom-left, bottom
    0x6D, // 3: top, top-right, middle, bottom-right, bottom
    0x2E, // 4: top-left, top-right, middle, bottom-right
    0x6B, // 5: top, top-left, middle, bottom-right, bottom
    0x7B, // 6: all but top-right
    0x25, // 7: top, top-right, bottom-right
    0x7F, // 8: all
    0x6F, // 9: all but bottom-left
];
/// Shown in every position when the clock cannot be read.
const SEG_DASH: u8 = 0x08;

const SEG_ON:  u32 = ACCENT;
const SEG_OFF: u32 = SURFACE_ALT;    // unlit segments stay faintly visible

/// Paint all seven segments of one digit, lit or unlit, so a new digit
/// fully replaces the old one.
fn draw_segments(win: &Window, x: i32, y: i32, segs: u8) {
    let bar_w  = DIGIT_W - 2 * SEG_T;          // horizontal segments
    let post_h = DIGIT_H / 2 - SEG_T;          // vertical segments
    let mid_y  = y + DIGIT_H / 2 - SEG_T / 2;
    let low_y  = y + DIGIT_H / 2 + SEG_T / 2;
    let right  = x + DIGIT_W - SEG_T;

    let seg = |bit: u8, sx: i32, sy: i32, sw: i32, sh: i32| {
        win.fill(sx, sy, sw, sh, if segs & bit != 0 { SEG_ON } else { SEG_OFF });
    };
    seg(0x01, x + SEG_T, y,                     bar_w, SEG_T);
    seg(0x02, x,         y + SEG_T,             SEG_T, post_h);
    seg(0x04, right,     y + SEG_T,             SEG_T, post_h);
    seg(0x08, x + SEG_T, mid_y,                 bar_w, SEG_T);
    seg(0x10, x,         low_y,                 SEG_T, post_h);
    seg(0x20, right,     low_y,                 SEG_T, post_h);
    seg(0x40, x + SEG_T, y + DIGIT_H - SEG_T,   bar_w, SEG_T);
}

/// Left edge of digit `slot`: 0-1 hours, 2-3 minutes, 4-5 seconds.
fn digit_x(slot: usize) -> i32 {
    let pair   = (slot / 2) as i32;
    let within = (slot % 2) as i32;
    // One pair plus the colon after it: D gap D gap colon gap.
    let pair_stride = 2 * DIGIT_W + COLON_W + 3 * DIGIT_GAP;
    CLOCK_X + pair * pair_stride + within * (DIGIT_W + DIGIT_GAP)
}

/// Left edge of colon 0 (after the hours) or 1 (after the minutes).
fn colon_x(idx: usize) -> i32 {
    digit_x(idx * 2 + 1) + DIGIT_W + DIGIT_GAP
}

fn draw_colon(win: &Window, x: i32) {
    let dot = SEG_T;
    let cx = x + (COLON_W - dot) / 2;
    win.fill(cx, CLOCK_Y + DIGIT_H / 3 - dot / 2,     dot, dot, SEG_ON);
    win.fill(cx, CLOCK_Y + 2 * DIGIT_H / 3 - dot / 2, dot, dot, SEG_ON);
}

// ── Drawing ─────────────────────────────────────────────────────────────────

/// Everything that does not change with the time.
fn draw_frame(win: &Window) {
    win.clear(BG);
    win.fill_rect(PANEL, SURFACE);
    win.rect(PANEL, BORDER);
    draw_colon(win, colon_x(0));
    draw_colon(win, colon_x(1));
    status_bar(win, b"Esc: close\0");
}

/// The line under the panel, centred.
fn draw_caption(win: &Window, text: &[u8], rgb: u32) {
    let row = Rect::new(PAD, DATE_Y, win.w() - 2 * PAD, FONT_H);
    win.fill_rect(row, BG);
    win.text_center(row, text, rgb);
}

/// A date and time of day: year, month, day, hour, minute, second.
type Stamp = [u32; 6];

fn draw_time(win: &Window, t: &Stamp) {
    let [year, month, day, hour, minute, second] = *t;
    let digits = [hour / 10, hour % 10, minute / 10, minute % 10, second / 10, second % 10];
    for (slot, &d) in digits.iter().enumerate() {
        draw_segments(win, digit_x(slot), CLOCK_Y, SEGS[d as usize % 10]);
    }

    // "YYYY-MM-DD"
    let two = |n: u32| [b'0' + (n / 10 % 10) as u8, b'0' + (n % 10) as u8];
    let (yh, yl, m, d) = (two(year / 100), two(year), two(month), two(day));
    let date = [yh[0], yh[1], yl[0], yl[1], b'-', m[0], m[1], b'-', d[0], d[1]];
    draw_caption(win, &date, FG_DIM);
}

fn draw_unavailable(win: &Window) {
    for slot in 0..6 {
        draw_segments(win, digit_x(slot), CLOCK_Y, SEG_DASH);
    }
    draw_caption(win, b"clock unavailable", RED);
}

// ── Time ────────────────────────────────────────────────────────────────────

/// The PIT ticks at 100 Hz.
const TICKS_PER_SEC: u32 = 100;

/// The RTC, with the time of day forced into range so it can always be
/// drawn. None when the kernel could not read it.
fn read_stamp() -> Option<Stamp> {
    let mut rtc = RtcTime { year: 0, month: 0, day: 0, hour: 0, minute: 0, second: 0 };
    if !read_clock(&mut rtc) {
        return None;
    }
    Some([rtc.year, rtc.month, rtc.day, rtc.hour % 24, rtc.minute % 60, rtc.second % 60])
}

/// `a` is at or after `b` on the wrapping tick counter.
fn tick_reached(a: u32, b: u32) -> bool {
    a.wrapping_sub(b) < 0x8000_0000
}

// ── Main ────────────────────────────────────────────────────────────────────

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    println!("rs-clock: start");
    let Some(mut win) = Window::open(b"Clock\0", WIN_W, WIN_H, 0) else {
        println!("rs-clock: could not open a window");
        quit(1);
    };

    draw_frame(&win);

    let mut shown: Option<Stamp> = None;     // what the window displays
    let mut next_look = ticks();             // tick at which to read the RTC again
    let mut last_change = next_look;         // tick at which the display last changed

    loop {
        let now = ticks();
        if tick_reached(now, next_look) {
            match read_stamp() {
                Some(t) if shown != Some(t) => {
                    draw_time(&win, &t);
                    win.flush();
                    // The very first reading falls anywhere inside its
                    // second, so keep looking every tick until the first
                    // rollover is seen. From then on the next rollover is
                    // one second away: sleep until one tick before it.
                    let ahead = if shown.is_none() { 1 } else { TICKS_PER_SEC - 1 };
                    next_look = now.wrapping_add(ahead);
                    last_change = now;
                    shown = Some(t);
                }
                Some(_) => {
                    // A little early: look again on the next tick. If the
                    // RTC has not moved for much longer than a second it is
                    // not running; then one look per second is enough.
                    let stalled = now.wrapping_sub(last_change) > TICKS_PER_SEC + TICKS_PER_SEC / 2;
                    next_look = now.wrapping_add(if stalled { TICKS_PER_SEC } else { 1 });
                }
                None => {
                    draw_unavailable(&win);
                    win.flush();
                    shown = None;
                    next_look = now.wrapping_add(TICKS_PER_SEC);
                }
            }
        }

        // Sleep until the next look is due. Input wakes the program early;
        // it then comes back here with the time that is left.
        let left = next_look.wrapping_sub(ticks());
        let wait_ticks = if left == 0 || left >= 0x8000_0000 { 1 } else { left };
        match win.wait(wait_ticks * (1000 / TICKS_PER_SEC)) {
            Some(Event::Close) | Some(Event::Key(KEY_ESC)) |
            Some(Event::Key(b'q')) | Some(Event::Key(b'Q')) => break,
            _ => {}
        }
    }

    win.close();
    println!("rs-clock: exit");
}
