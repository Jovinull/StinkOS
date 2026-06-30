//! rs-net: network status viewer for StinkOS.
//!
//! Shows the ARP cache (IP→MAC mappings) and refreshes live.
//! Press R to refresh, Q/Esc to exit.

#![no_std]
#![no_main]

use libstink::println;
use libui::*;

const SCREEN_W: i32 = 1024;
const SCREEN_H: i32 = 768;
const WIN_W: i32 = 700;
const WIN_H: i32 = 480;
const WIN_X: i32 = (SCREEN_W - WIN_W) / 2;
const WIN_Y: i32 = (SCREEN_H - WIN_H) / 2;

const CONTENT_X: i32 = WIN_X + 16;
const CONTENT_Y: i32 = WIN_Y + 48;
const LINE_H: i32 = 20;

fn draw_lines(buf: &[u8], n: i32) {
    let mut y = CONTENT_Y;
    let mut start = 0usize;
    let mut is_header = true;
    for i in 0..n as usize {
        if buf[i] == b'\n' || i + 1 == n as usize {
            let end = if buf[i] == b'\n' { i } else { i + 1 };
            if end > start {
                let mut tmp = [0u8; 80];
                let l = (end - start).min(79);
                tmp[..l].copy_from_slice(&buf[start..start + l]);
                tmp[l] = 0;
                let color = if is_header { FG_DIM } else { FG };
                text16(CONTENT_X, y, &tmp, color);
                is_header = false;
            }
            y += LINE_H;
            start = i + 1;
        }
    }
}

fn render(arp: &[u8], arp_n: i32, proc_buf: &[u8], proc_n: i32) {
    fill(0, 0, SCREEN_W, SCREEN_H, BG);
    window_frame(WIN_X, WIN_Y, WIN_W, WIN_H, b"Network Status\0");

    // Section: ARP cache
    text16(CONTENT_X, WIN_Y + 36, b"ARP Cache\0", ACCENT);
    fill(CONTENT_X, WIN_Y + 45, WIN_W - 32, 1, BORDER);

    if arp_n <= 4 {
        // Only header, no entries
        let empty_y = CONTENT_Y + LINE_H;
        text16(CONTENT_X, empty_y, b"(no ARP entries -- no active network connections)\0", FG_DIM);
    } else {
        draw_lines(arp, arp_n);
    }

    // Section: process count (quick summary)
    let section2_y = CONTENT_Y + LINE_H * 6 + 10;
    text16(CONTENT_X, section2_y - 2, b"Processes\0", ACCENT);
    fill(CONTENT_X, section2_y + 7, WIN_W - 32, 1, BORDER);

    // Count procs from proc_info output
    let mut pcount = 0u32;
    for i in 0..proc_n as usize {
        if i > 0 && proc_buf[i] == b'\n' { pcount += 1; }
    }
    if pcount > 0 { pcount -= 1; } // remove header

    let mut pbuf = [0u8; 12];
    fmt_u32(pcount, &mut pbuf);
    text16(CONTENT_X, section2_y + 14, b"Running: \0", FG_DIM);
    text16(CONTENT_X + 72, section2_y + 14, &pbuf, FG);

    // Uptime
    let tick = ticks();
    let sec = tick / 1000;
    let mut tbuf = [0u8; 9];
    fmt_hhmmss(sec / 3600, (sec % 3600) / 60, sec % 60, &mut tbuf);
    text16(CONTENT_X, section2_y + 34, b"Uptime:  \0", FG_DIM);
    text16(CONTENT_X + 72, section2_y + 34, &tbuf, FG);

    // Wall clock
    let mut rtc = RtcTime { year: 0, month: 0, day: 0, hour: 0, minute: 0, second: 0 };
    if read_clock(&mut rtc) == 0 {
        let mut cbuf = [0u8; 9];
        fmt_hhmmss(rtc.hour, rtc.minute, rtc.second, &mut cbuf);
        text16(CONTENT_X, section2_y + 54, b"Clock:   \0", FG_DIM);
        text16(CONTENT_X + 72, section2_y + 54, &cbuf, FG);
    }

    // Hint
    let hint_y = WIN_Y + WIN_H - 18;
    fill(WIN_X + 8, hint_y - 2, WIN_W - 16, 1, BORDER);
    text16(CONTENT_X, hint_y + 2, b"R refresh    Q quit\0", FG_DIM);
}

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    println!("rs-net: start");

    let mut arp_buf  = [0u8; 512];
    let mut proc_buf = [0u8; 1024];
    let mut prev_k   = 0i32;
    let mut dirty    = true;
    let mut arp_n    = 0i32;
    let mut proc_n   = 0i32;

    // Initial fetch
    arp_n  = arp_info(&mut arp_buf);
    proc_n = proc_info(&mut proc_buf);

    loop {
        if dirty {
            render(&arp_buf, arp_n, &proc_buf, proc_n);
            dirty = false;
        }

        let k = poll_key();
        if k != 0 && k != prev_k {
            match k {
                k if k == b'q' as i32 || k == 27 => break,
                k if k == b'r' as i32 || k == b'R' as i32 => {
                    arp_n  = arp_info(&mut arp_buf);
                    proc_n = proc_info(&mut proc_buf);
                    dirty  = true;
                }
                _ => {}
            }
        }
        prev_k = k;

        sleep_ms(200);
    }

    println!("rs-net: exit");
}
