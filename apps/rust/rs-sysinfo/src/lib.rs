//! rs-sysinfo: system information viewer for StinkOS.
//!
//! Displays uptime, current time, and the live process table inside a
//! windowed frame. Press Q to exit. F11 to toggle maximize.

#![no_std]
#![no_main]

use libstink::println;
use libui::*;

const SCREEN_W: i32 = 1024;
const SCREEN_H: i32 = 768;
const WIN_X: i32 = 80;
const WIN_Y: i32 = 60;
const WIN_W: i32 = SCREEN_W - 160;
const WIN_H: i32 = SCREEN_H - 120;

fn kv(x: i32, y: i32, label: &[u8], val: &[u8]) {
    text16(x, y, label, FG_DIM);
    let lw = (nul_len(label) as i32) * 8;
    text16(x + lw, y, val, FG);
}

fn draw_lines(buf: &[u8], len: usize, x: i32, start_y: i32, max_y: i32,
              header_rgb: u32, body_rgb: u32) -> i32 {
    let mut y = start_y;
    let mut line_start = 0usize;
    let mut first = true;
    let row_h = 18i32;
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
                text16(x, y, &lbuf, rgb);
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
    let mut cw = WIN_W;
    let mut ch = WIN_H;
    let mut maximized = false;
    let mut prev_k = 0i32;
    win_init_at(b"System Info \0", WIN_W, WIN_H, WIN_X, WIN_Y);

    loop {
        let cx    = 16;
        let max_y = ch - 16;

        fill(0, 0, cw, ch, BG);
        let content_y = window_frame(0, 0, cw, ch, b"System Information\0");
        let mut cy = content_y + 16;

        let t    = ticks();
        let secs = t / 1000;
        let mut ubuf = [0u8; 9];
        fmt_hhmmss(secs / 3600, (secs % 3600) / 60, secs % 60, &mut ubuf);
        kv(cx, cy, b"Uptime : \0", &ubuf);
        cy += 16;

        let mut rtc = RtcTime { year: 0, month: 0, day: 0, hour: 0, minute: 0, second: 0 };
        if read_clock(&mut rtc) == 0 {
            let mut cbuf = [0u8; 9];
            fmt_hhmmss(rtc.hour, rtc.minute, rtc.second, &mut cbuf);
            kv(cx, cy, b"Time   : \0", &cbuf);
            cy += 16;
        }

        cy += 10;

        let mut pbuf = [0u8; 2048];
        let plen = proc_info(&mut pbuf);
        if plen > 0 {
            text16(cx, cy, b"Processes\0", FG_DIM);
            cy += 14;
            fill(cx, cy, cw - 32, 1, BORDER);
            cy += 6;
            cy = draw_lines(&pbuf, plen as usize, cx, cy, max_y, BORDER, FG);
        }

        let footer_y = ch - 20;
        text16(cx, footer_y, b"Q quit  F11 max\0", FG_DIM);

        let k = poll_key();
        if k != 0 && k != prev_k {
            match k {
                k if k == b'q' as i32 || k == 27 => break,
                KEY_F11 => {
                    maximized = !maximized;
                    let (nw, nh) = if maximized { (SCREEN_W_FULL, SCREEN_H_FULL) } else { (WIN_W, WIN_H) };
                    if win_resize(nw, nh) { cw = nw; ch = nh; }
                }
                _ => {}
            }
        }
        prev_k = k;
        if let Some(ev) = win_poll_event() {
            if ev.kind == WIN_EV_RESIZE && win_resize(ev.x, ev.y) {
                cw = ev.x; ch = ev.y;
            }
        }

        win_flush();
        sleep_ms(500);
    }

    win_done();
    println!("rs-sysinfo: exit");
}
