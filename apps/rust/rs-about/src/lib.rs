//! rs-about: "About StinkOS" information screen.
//!
//! Shows OS version, architecture, kernel info, and a brief feature list.
//! Reads live uptime + wall clock from the kernel. Press Q or Esc to exit.
//! Press F11 to toggle maximize / restore.

#![no_std]
#![no_main]

use libstink::println;
use libui::*;

const SCREEN_W: i32 = 1024;
const SCREEN_H: i32 = 768;
const WIN_W: i32 = 640;
const WIN_H: i32 = 480;
const WIN_X: i32 = (SCREEN_W - WIN_W) / 2;
const WIN_Y: i32 = (SCREEN_H - WIN_H) / 2;

const CONTENT_X: i32 = 24;
const CONTENT_Y: i32 = 52;
const LINE_H: i32 = 20;

static LINES: &[(&[u8], u32)] = &[
    (b"StinkOS\0",                              0x57f287),
    (b"An original x86 32-bit hobby OS\0",      0xe6edf3),
    (b"\0",                                     0x000000),
    (b"Architecture   x86 (i686), 32-bit protected mode\0",   0x8b949e),
    (b"Video          VBE 1024x768 32bpp linear framebuffer\0",0x8b949e),
    (b"Kernel         Ring 0 C kernel, PAE paging, COW fork\0",0x8b949e),
    (b"Userland       Ring 3 ELF processes, 80+ syscalls\0",   0x8b949e),
    (b"Filesystem     StinkFS flat-file raw-sector layout\0",  0x8b949e),
    (b"Network        TCP/IP stack, Ethernet, UDP\0",          0x8b949e),
    (b"Audio          PC speaker beeper\0",                    0x8b949e),
    (b"\0",                                                    0x000000),
    (b"Built with     i386-elf-gcc cross-compiler + NASM\0",   0x8b949e),
    (b"Emulation      QEMU (test), bare metal (target)\0",     0x8b949e),
    (b"\0",                                                    0x000000),
    (b"Apps           Desktop, Files, Editor, Paint, Calc\0",  0x8b949e),
    (b"               Sysinfo, Snake, Pong, Asteroids, Doom\0",0x8b949e),
];

fn draw(tick: u32, cw: i32, ch: i32) {
    fill(0, 0, cw, ch, BG);
    window_frame(0, 0, cw, ch, b"About StinkOS\0");

    fill(CONTENT_X - 4, CONTENT_Y - 6, cw - 32, 1, BORDER);

    let mut y = CONTENT_Y;
    for (line, color) in LINES.iter() {
        if !line.is_empty() && line[0] != 0 {
            text16(CONTENT_X, y, line, *color);
        }
        y += LINE_H;
    }

    let stats_y = ch - 80;
    fill(8, stats_y - 4, cw - 16, 1, BORDER);

    let uptime_sec = tick / 1000;
    let mut tbuf = [0u8; 9];
    fmt_hhmmss(uptime_sec / 3600, (uptime_sec % 3600) / 60, uptime_sec % 60, &mut tbuf);
    text16(CONTENT_X, stats_y + 4, b"Uptime  \0", FG_DIM);
    text16(CONTENT_X + 72, stats_y + 4, &tbuf, FG);

    let mut rtc = RtcTime { year: 0, month: 0, day: 0, hour: 0, minute: 0, second: 0 };
    if read_clock(&mut rtc) == 0 {
        let mut cbuf = [0u8; 9];
        fmt_hhmmss(rtc.hour, rtc.minute, rtc.second, &mut cbuf);
        text16(CONTENT_X, stats_y + 24, b"Clock   \0", FG_DIM);
        text16(CONTENT_X + 72, stats_y + 24, &cbuf, FG);
    }

    text16(cw - 128, ch - 24, b"Q close F11 max\0", FG_DIM);
}

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    println!("rs-about: start");
    win_init_at(b"About StinkOS\0", WIN_W, WIN_H, WIN_X, WIN_Y);

    let mut cw = WIN_W;
    let mut ch = WIN_H;
    let mut maximized = false;
    let mut prev_k = 0i32;

    loop {
        draw(ticks(), cw, ch);
        win_flush();

        let k = poll_key();
        if k != 0 && k != prev_k {
            match k {
                k if k == b'q' as i32 || k == b'Q' as i32 || k == 27 => break,
                KEY_F11 => {
                    maximized = !maximized;
                    let (nw, nh) = if maximized {
                        (SCREEN_W_FULL, SCREEN_H_FULL)
                    } else {
                        (WIN_W, WIN_H)
                    };
                    if win_resize(nw, nh) { cw = nw; ch = nh; }
                }
                _ => {}
            }
        }
        prev_k = k;

        sleep_ms(500);
    }

    win_done();
    println!("rs-about: exit");
}
