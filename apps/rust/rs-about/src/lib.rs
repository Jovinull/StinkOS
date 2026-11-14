//! rs-about: what StinkOS is, plus live uptime and wall clock.
//!
//! The simplest shape of a windowed program: open a window, draw, wait for
//! the next event with a timeout (so the clock ticks), redraw. Esc, Q or the
//! close box end it.

#![no_std]
#![no_main]

use libstink::println;
use libui::*;

const WIN_W: i32 = 520;
const WIN_H: i32 = 372;

static FACTS: &[(&[u8], &[u8])] = &[
    (b"Architecture\0", b"x86, 32-bit protected mode, PAE\0"),
    (b"Kernel\0",       b"C + assembly, higher half, W^X\0"),
    (b"Processes\0",    b"ring 3 ELF, fork with copy-on-write\0"),
    (b"Filesystem\0",   b"StinkFS on ATA\0"),
    (b"Network\0",      b"e1000, ARP IPv4 ICMP UDP TCP DHCP DNS\0"),
    (b"Audio\0",        b"Sound Blaster 16 mixer, PC speaker\0"),
    (b"Video\0",        b"VBE linear framebuffer, kernel compositor\0"),
    (b"Userland\0",     b"C (libstink) and Rust (libui), no libc\0"),
];

fn draw(win: &Window) {
    win.clear(BG);

    win.text(PAD, PAD, b"StinkOS\0", ACCENT);
    win.text(PAD, PAD + ROW_H, b"An x86 operating system written from scratch.\0", FG);

    let mut y = section(win, PAD, PAD + 3 * ROW_H, win.w() - 2 * PAD, b"System\0");
    for (label, value) in FACTS {
        win.text(PAD, y, label, FG_DIM);
        win.text_fit(PAD + 15 * FONT_W, y, value, win.w() - 2 * PAD - 15 * FONT_W, FG);
        y += ROW_H;
    }

    let y = section(win, PAD, y + GAP, win.w() - 2 * PAD, b"Now\0");
    // The PIT ticks at 100 Hz.
    let up = ticks() / 100;
    let mut buf = [0u8; 9];
    fmt_hhmmss(up / 3600, (up / 60) % 60, up % 60, &mut buf);
    field(win, PAD, y, PAD + 30 * FONT_W, b"Uptime\0", &buf, FG);

    let mut rtc = RtcTime { year: 0, month: 0, day: 0, hour: 0, minute: 0, second: 0 };
    if read_clock(&mut rtc) {
        fmt_hhmmss(rtc.hour, rtc.minute, rtc.second, &mut buf);
        field(win, PAD, y + ROW_H, PAD + 30 * FONT_W, b"Clock\0", &buf, FG);
    }

    status_bar(win, b"Esc: close\0");
}

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    println!("rs-about: start");
    let Some(mut win) = Window::open(b"About StinkOS\0", WIN_W, WIN_H, 0) else {
        println!("rs-about: could not open a window");
        quit(1);
    };

    loop {
        draw(&win);
        win.flush();
        // Wake once a second for the clock, or sooner for input.
        match win.wait(1000) {
            Some(Event::Close) | Some(Event::Key(KEY_ESC)) | Some(Event::Key(b'q')) => break,
            _ => {}
        }
    }

    win.close();
    println!("rs-about: exit");
}
