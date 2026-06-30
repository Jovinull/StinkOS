//! rs-cal: monthly calendar viewer for StinkOS.
//!
//! Reads current date from RTC. Displays a classic month grid.
//! Left/Right: previous/next month. Q/Esc: quit.

#![no_std]
#![no_main]

use libstink::println;
use libui::*;

const SCREEN_W: i32 = 1024;
const SCREEN_H: i32 = 768;
const WIN_W: i32 = 560;
const WIN_H: i32 = 420;
const WIN_X: i32 = (SCREEN_W - WIN_W) / 2;
const WIN_Y: i32 = (SCREEN_H - WIN_H) / 2;

const CX: i32 = WIN_X + 16;

// ── Calendar math ─────────────────────────────────────────────────────────────

fn is_leap(y: u32) -> bool {
    (y % 4 == 0 && y % 100 != 0) || y % 400 == 0
}

fn days_in_month(m: u32, y: u32) -> u32 {
    match m {
        1 | 3 | 5 | 7 | 8 | 10 | 12 => 31,
        4 | 6 | 9 | 11 => 30,
        2 => if is_leap(y) { 29 } else { 28 },
        _ => 30,
    }
}

// Zeller-ish: weekday of 1st of given month (0=Sun…6=Sat).
// Uses Tomohiko Sakamoto's algorithm (public domain).
fn weekday_of_first(m: u32, y: u32) -> u32 {
    const T: [u32; 12] = [0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4];
    let mut yr = y;
    if m < 3 { yr -= 1; }
    let d = 1u32;
    (yr + yr / 4 - yr / 100 + yr / 400 + T[(m as usize) - 1] + d) % 7
}

// ── Rendering ─────────────────────────────────────────────────────────────────

const MONTH_NAMES: [&[u8]; 13] = [
    b"", b"January\0", b"February\0", b"March\0", b"April\0",
    b"May\0", b"June\0", b"July\0", b"August\0", b"September\0",
    b"October\0", b"November\0", b"December\0",
];

fn draw_num_right(x: i32, y: i32, n: u32, color: u32, field_w: i32) {
    let mut buf = [0u8; 12];
    let len = fmt_u32(n, &mut buf) as i32;
    text16(x + field_w - len * 8, y, &buf, color);
}

fn draw_year_month(y: u32, m: u32) {
    let mut buf = [0u8; 12];
    let yl = fmt_u32(y, &mut buf);
    buf[yl] = 0;
    text16(CX, WIN_Y + 34 + 10, MONTH_NAMES[m as usize], ACCENT);
    text16(CX + 120, WIN_Y + 34 + 10, &buf, FG);
}

fn draw_grid(month: u32, year: u32, today_day: u32, today_month: u32, today_year: u32) {
    let content_y = WIN_Y + 34;
    let grid_top  = content_y + 32;

    // Day-of-week header
    let day_names = [b"Su\0", b"Mo\0", b"Tu\0", b"We\0", b"Th\0", b"Fr\0", b"Sa\0"];
    let cell_w = (WIN_W - 32) / 7;
    let cell_h = 42i32;

    fill(WIN_X + 1, grid_top, WIN_W - 2, 16, SURFACE);
    for d in 0..7usize {
        let hx = CX + d as i32 * cell_w;
        let col = if d == 0 || d == 6 { 0x7b8a9a } else { FG_DIM };
        text16(hx + (cell_w - 16) / 2, grid_top + 4, day_names[d], col);
    }
    fill(WIN_X + 1, grid_top + 16, WIN_W - 2, 1, BORDER);

    let first_wd = weekday_of_first(month, year) as i32;
    let days      = days_in_month(month, year);
    let mut col   = first_wd;
    let mut row   = 0i32;

    for day in 1..=days {
        let cx2 = CX + col * cell_w;
        let cy2 = grid_top + 18 + row * cell_h;

        let is_today = (day == today_day) && (month == today_month) && (year == today_year);
        let is_we    = col == 0 || col == 6;

        if is_today {
            fill(cx2 - 2, cy2 - 2, cell_w - 2, cell_h - 4, ACCENT);
            draw_num_right(cx2, cy2 + (cell_h - 16) / 2 - 2, day, BG, cell_w - 4);
        } else {
            let c = if is_we { 0x7b8a9a } else { FG };
            draw_num_right(cx2, cy2 + (cell_h - 16) / 2 - 2, day, c, cell_w - 4);
        }

        col += 1;
        if col == 7 { col = 0; row += 1; }
    }
}

fn redraw(month: u32, year: u32, today_day: u32, today_month: u32, today_year: u32) {
    fill(0, 0, SCREEN_W, SCREEN_H, BG);
    window_frame(WIN_X, WIN_Y, WIN_W, WIN_H, b"Calendar\0");
    fill(WIN_X + 1, WIN_Y + 34, WIN_W - 2, WIN_H - 35, SURFACE);

    draw_year_month(year, month);
    draw_grid(month, year, today_day, today_month, today_year);

    // Footer
    let fy = WIN_Y + WIN_H - 22;
    fill(WIN_X + 1, fy - 4, WIN_W - 2, 1, BORDER);
    text16(CX, fy, b"Left/Right: month   Q: quit\0", FG_DIM);
}

// ── Main ──────────────────────────────────────────────────────────────────────

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    println!("rs-cal: start");

    let mut rtc = RtcTime { year: 0, month: 0, day: 0, hour: 0, minute: 0, second: 0 };
    read_clock(&mut rtc);

    let today_year  = if rtc.year  > 0 { rtc.year  as u32 } else { 2025 };
    let today_month = if rtc.month > 0 { rtc.month as u32 } else { 1 };
    let today_day   = if rtc.day   > 0 { rtc.day   as u32 } else { 1 };

    let mut view_year  = today_year;
    let mut view_month = today_month;
    let mut dirty = true;
    let mut prev_k = 0i32;

    loop {
        if dirty {
            redraw(view_month, view_year, today_day, today_month, today_year);
            dirty = false;
        }

        let k = poll_key();
        if k != 0 && k != prev_k {
            match k {
                k if k == b'q' as i32 || k == 27 => break,
                k if k == KEY_LEFT => {
                    if view_month == 1 { view_month = 12; view_year -= 1; }
                    else               { view_month -= 1; }
                    dirty = true;
                }
                k if k == KEY_RIGHT => {
                    if view_month == 12 { view_month = 1; view_year += 1; }
                    else                { view_month += 1; }
                    dirty = true;
                }
                _ => {}
            }
        }
        prev_k = k;
        sleep_ms(16);
    }

    println!("rs-cal: exit");
}
