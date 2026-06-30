//! rs-sysinfo: system information viewer for StinkOS.
//!
//! Displays uptime, current time, and the live process table inside a
//! windowed frame.  Press Q to exit back to the desktop.

#![no_std]
#![no_main]

use libstink::println;
use libui::*;

const SCREEN_W: i32 = 1024;
const SCREEN_H: i32 = 768;
const WIN_X:    i32 = 80;
const WIN_Y:    i32 = 60;
const WIN_W:    i32 = SCREEN_W - 160;
const WIN_H:    i32 = SCREEN_H - 120;

/// Draw a labelled value row at (x, y).
fn kv(x: i32, y: i32, label: &[u8], val: &[u8]) {
    text(x, y, label, FG_DIM);
    let lw = (nul_len(label) as i32) * 8;
    text(x + lw, y, val, FG);
}

/// Split buf[0..len] into '\n'-delimited lines and draw them, returning the
/// new Y position.  Stops before hitting max_y.
fn draw_lines(buf: &[u8], len: usize, x: i32, start_y: i32, max_y: i32,
              header_rgb: u32, body_rgb: u32) -> i32 {
    let mut y = start_y;
    let mut line_start = 0usize;
    let mut first = true;
    let row_h = 14i32;

    for i in 0..=len {
        let at_end = i == len;
        let is_nl  = !at_end && buf[i] == b'\n';
        if is_nl || at_end {
            let end = i;
            let llen = if end > line_start { end - line_start } else { 0 };
            if llen > 0 && y + row_h <= max_y {
                let mut lbuf = [0u8; 64];
                let cp = if llen > 63 { 63 } else { llen };
                for j in 0..cp { lbuf[j] = buf[line_start + j]; }
                lbuf[cp] = 0;
                let rgb = if first { header_rgb } else { body_rgb };
                text(x, y, &lbuf, rgb);
                y += row_h;
                first = false;
            }
            line_start = i + 1;
        }
    }
    y
}

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    println!("rs-sysinfo: start");

    fill(0, 0, SCREEN_W, SCREEN_H, BG);

    let cx     = WIN_X + 16;
    let max_y  = WIN_Y + WIN_H - 16;
    let mut prev_k = 0i32;

    loop {
        /* Redraw full window */
        fill(0, 0, SCREEN_W, SCREEN_H, BG);
        let content_y = window_frame(WIN_X, WIN_Y, WIN_W, WIN_H, b"System Information\0");
        let mut cy = content_y + 16;

        /* ── Uptime ── */
        let t      = ticks();
        let secs   = t / 1000;
        let h_up   = secs / 3600;
        let m_up   = (secs % 3600) / 60;
        let s_up   = secs % 60;
        let mut ubuf = [0u8; 9];
        fmt_hhmmss(h_up, m_up, s_up, &mut ubuf);
        kv(cx, cy, b"Uptime : \0", &ubuf);
        cy += 16;

        /* ── Wall clock ── */
        let mut rtc = RtcTime { year: 0, month: 0, day: 0, hour: 0, minute: 0, second: 0 };
        if read_clock(&mut rtc) == 0 {
            let mut cbuf = [0u8; 9];
            fmt_hhmmss(rtc.hour, rtc.minute, rtc.second, &mut cbuf);
            kv(cx, cy, b"Time   : \0", &cbuf);
            cy += 16;
        }

        cy += 10;

        /* ── Process table ── */
        let mut pbuf = [0u8; 2048];
        let plen = proc_info(&mut pbuf);
        if plen > 0 {
            text(cx, cy, b"Processes\0", FG_DIM);
            cy += 14;
            fill(cx, cy, WIN_W - 32, 1, BORDER);
            cy += 6;
            cy = draw_lines(&pbuf, plen as usize, cx, cy, max_y, BORDER, FG);
        }

        /* ── Footer ── */
        let footer_y = WIN_Y + WIN_H - 20;
        text(cx, footer_y, b"Q - quit\0", FG_DIM);

        let k = poll_key();
        if k != 0 && k != prev_k && (k == b'q' as i32 || k == 27) { break; }
        prev_k = k;
        sleep_ms(500);
    }

    println!("rs-sysinfo: exit");
}
