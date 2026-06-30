//! rs-clock: 7-segment digital clock for StinkOS.
//!
//! Renders HH:MM:SS using simulated 7-segment display drawn with sys_fillrect.
//! Updates once per second; only redraws digits that changed.
//! Press Q or Esc to exit.

#![no_std]
#![no_main]

use libstink::println;
use libui::*;

// ── Window layout ─────────────────────────────────────────────────────────────

const SCREEN_W: i32 = 1024;
const SCREEN_H: i32 = 768;
const WIN_W:    i32 = 500;
const WIN_H:    i32 = 220;
const WIN_X:    i32 = (SCREEN_W - WIN_W) / 2;
const WIN_Y:    i32 = (SCREEN_H - WIN_H) / 2;

// ── 7-segment geometry ────────────────────────────────────────────────────────

const DW:  i32 = 50;  // digit width
const DH:  i32 = 90;  // digit height
const ST:  i32 = 6;   // segment thickness
const GAP: i32 = 8;   // gap between elements
const CW:  i32 = 16;  // colon width

// Total clock strip width: 6 digits + 5 inter-digit gaps + 2 colons + 4 gaps around colons
// HH : MM : SS  →  D D . D D . D D
// Spaces: D(gap)D(gap)C(gap)D(gap)D(gap)C(gap)D(gap)D
const CLOCK_W: i32 = 6 * DW + 7 * GAP + 2 * CW; // 300+56+32 = 388
const CLOCK_X: i32 = WIN_X + (WIN_W - CLOCK_W) / 2;
const CLOCK_Y: i32 = WIN_Y + 34 + (WIN_H - 34 - DH) / 2;

// ── 7-segment patterns ────────────────────────────────────────────────────────
// bit0=Top bit1=TL bit2=TR bit3=Mid bit4=BL bit5=BR bit6=Bot

const SEGS: [u8; 10] = [
    0x77, // 0: top, TL, TR, BL, BR, bot
    0x24, // 1: TR, BR
    0x5D, // 2: top, TR, mid, BL, bot
    0x6D, // 3: top, TR, mid, BR, bot
    0x2E, // 4: TL, TR, mid, BR
    0x6B, // 5: top, TL, mid, BR, bot
    0x7B, // 6: top, TL, mid, BL, BR, bot
    0x25, // 7: top, TR, BR
    0x7F, // 8: all
    0x6F, // 9: top, TL, TR, mid, BR, bot
];

fn draw_digit(x: i32, y: i32, d: u8, color_on: u32, color_off: u32) {
    let s = SEGS[d as usize];
    let vert_h = DH / 2 - ST;

    let seg = |on: bool, fx: i32, fy: i32, fw: i32, fh: i32| {
        fill(fx, fy, fw, fh, if on { color_on } else { color_off });
    };

    // Top
    seg(s & 0x01 != 0, x + ST,        y,              DW - 2*ST, ST);
    // TL
    seg(s & 0x02 != 0, x,              y + ST,         ST, vert_h);
    // TR
    seg(s & 0x04 != 0, x + DW - ST,   y + ST,         ST, vert_h);
    // Mid
    seg(s & 0x08 != 0, x + ST,        y + DH/2 - ST/2, DW - 2*ST, ST);
    // BL
    seg(s & 0x10 != 0, x,              y + DH/2 + ST/2, ST, vert_h);
    // BR
    seg(s & 0x20 != 0, x + DW - ST,   y + DH/2 + ST/2, ST, vert_h);
    // Bot
    seg(s & 0x40 != 0, x + ST,        y + DH - ST,    DW - 2*ST, ST);
}

fn draw_colon(x: i32, y: i32, color: u32) {
    let dot_sz = 6;
    let cx = x + CW / 2 - dot_sz / 2;
    fill(cx, y + DH / 3 - dot_sz / 2,     dot_sz, dot_sz, color);
    fill(cx, y + 2 * DH / 3 - dot_sz / 2, dot_sz, dot_sz, color);
}

// ── Digit position helpers ────────────────────────────────────────────────────

fn digit_x(slot: usize) -> i32 {
    // Slots: 0=H1 1=H2  (colon)  2=M1 3=M2  (colon)  4=S1 5=S2
    let pair   = slot / 2; // 0, 1, 2
    let within = slot % 2; // 0 or 1
    CLOCK_X + pair as i32 * (2 * DW + 2 * GAP + CW + GAP)
             + within as i32 * (DW + GAP)
}

fn colon_x(idx: usize) -> i32 {
    // Colon 0 between H and M, colon 1 between M and S
    digit_x(idx * 2 + 1) + DW + GAP
}

// ── Rendering ─────────────────────────────────────────────────────────────────

const SEG_ON:  u32 = ACCENT;
const SEG_OFF: u32 = 0x1a2030; // dark segment background

fn draw_clock(h: u32, m: u32, s: u32) {
    let digits = [
        (h / 10) as u8, (h % 10) as u8,
        (m / 10) as u8, (m % 10) as u8,
        (s / 10) as u8, (s % 10) as u8,
    ];
    for (i, &d) in digits.iter().enumerate() {
        draw_digit(digit_x(i), CLOCK_Y, d, SEG_ON, SEG_OFF);
    }
    draw_colon(colon_x(0), CLOCK_Y, ACCENT);
    draw_colon(colon_x(1), CLOCK_Y, ACCENT);
}

fn draw_date(year: u32, month: u32, day: u32) {
    // "YYYY-MM-DD" centered under the clock
    let mut buf = [0u8; 12];
    let y4 = [
        b'0' + (year / 1000 % 10) as u8,
        b'0' + (year / 100  % 10) as u8,
        b'0' + (year / 10   % 10) as u8,
        b'0' + (year        % 10) as u8,
    ];
    buf[0] = y4[0]; buf[1] = y4[1]; buf[2] = y4[2]; buf[3] = y4[3];
    buf[4] = b'-';
    buf[5] = b'0' + (month / 10 % 10) as u8;
    buf[6] = b'0' + (month % 10) as u8;
    buf[7] = b'-';
    buf[8] = b'0' + (day / 10 % 10) as u8;
    buf[9] = b'0' + (day % 10) as u8;
    buf[10] = 0;
    let dw = 10 * 8;
    let dx = WIN_X + (WIN_W - dw) / 2;
    let dy = CLOCK_Y + DH + 10;
    fill(WIN_X + 16, dy - 2, WIN_W - 32, 10 + 2, SURFACE);
    text16(dx, dy, &buf, FG_DIM);
}

// ── Main ──────────────────────────────────────────────────────────────────────

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    println!("rs-clock: start");

    fill(0, 0, SCREEN_W, SCREEN_H, BG);
    window_frame(WIN_X, WIN_Y, WIN_W, WIN_H, b"Digital Clock\0");
    fill(WIN_X + 8, WIN_Y + 34, WIN_W - 16, WIN_H - 42, SURFACE);

    let mut prev_h = 99u32;
    let mut prev_m = 99u32;
    let mut prev_s = 99u32;

    loop {
        let k = poll_key();
        if k == b'q' as i32 || k == b'Q' as i32 || k == 27 { break; }

        let mut rtc = RtcTime { year: 0, month: 0, day: 0, hour: 0, minute: 0, second: 0 };
        if read_clock(&mut rtc) == 0 {
            let h = rtc.hour % 24;
            let m = rtc.minute % 60;
            let s = rtc.second % 60;

            if h != prev_h || m != prev_m || s != prev_s {
                draw_clock(h, m, s);
                draw_date(rtc.year, rtc.month, rtc.day);
                prev_h = h; prev_m = m; prev_s = s;
            }
        }

        sleep_ms(200); // poll at 5 Hz; RTC only updates every second anyway
    }

    println!("rs-clock: exit");
}
