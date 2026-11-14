//! rs-net: what the network stack knows right now.
//!
//! Two panels, both read from the kernel once a second: the interface
//! (link, MAC, DHCP state and the addresses the lease gave us) and the ARP
//! cache. Nothing here is cached or assumed -- with no NIC, or before DHCP
//! binds, the panel says so. R refreshes at once; Esc, Q or the close box
//! end it.

#![no_std]
#![no_main]

use libstink::println;
use libui::*;

/// ARP rows the window keeps room for (the kernel's cache holds 16).
const ARP_ROWS: usize = 8;
const ARP_CAP: usize = 1024;

const LABEL_W: i32 = 12 * FONT_W;
const WIN_W: i32 = 2 * PAD + 44 * FONT_W;
const SECTION_H: i32 = FONT_H + 3 + GAP;
const WIN_H: i32 = PAD + SECTION_H + 7 * ROW_H                 // Interface
                 + GAP + SECTION_H + (ARP_ROWS as i32 + 1) * ROW_H   // ARP: titles + rows
                 + GAP + STATUS_H;

fn dhcp_text(state: u8) -> (&'static [u8], u32) {
    match state {
        0 => (b"not started\0", FG_DIM),
        1 => (b"discovering\0", YELLOW),
        2 => (b"requesting\0", YELLOW),
        3 => (b"bound\0", ACCENT),
        4 => (b"failed\0", RED),
        _ => (b"unknown\0", FG_DIM),
    }
}

/// One address row; an all-zero address is shown as "not set".
fn addr_row(win: &Window, y: i32, label: &[u8], ip: u32) {
    win.text(PAD, y, label, FG_DIM);
    if ip == 0 {
        win.text(PAD + LABEL_W, y, b"not set\0", FG_DIM);
    } else {
        let mut buf = [0u8; 16];
        fmt_ip(ip, &mut buf);
        win.text(PAD + LABEL_W, y, &buf, FG);
    }
}

/// The ARP snapshot is text: a title line, then "IP<spaces>MAC" per entry.
/// Returns the two fields of an entry line, or None for anything else.
fn arp_entry(line: &[u8]) -> Option<(&[u8], &[u8])> {
    if line.is_empty() || !line[0].is_ascii_digit() {
        return None;
    }
    let ip_end = line.iter().position(|&b| b == b' ')?;
    let mac_start = ip_end + line[ip_end..].iter().position(|&b| b != b' ')?;
    Some((&line[..ip_end], &line[mac_start..]))
}

fn draw(win: &Window) {
    win.clear(BG);
    let w = win.w() - 2 * PAD;
    let ty = (ROW_H - FONT_H) / 2;

    // ── Interface ──
    let mut y = section(win, PAD, PAD, w, b"Interface\0");
    match netinfo() {
        None => {
            win.text_fit(PAD, y + ty, b"The kernel did not report the network state.\0", w, RED);
        }
        Some(ni) => {
            win.text(PAD, y + ty, b"Link\0", FG_DIM);
            if ni.link_up != 0 {
                win.text(PAD + LABEL_W, y + ty, b"up (e1000)\0", ACCENT);
            } else {
                win.text(PAD + LABEL_W, y + ty, b"no network card\0", FG_DIM);
            }
            y += ROW_H;

            let mut mac = [0u8; 18];
            fmt_mac(&ni.mac, &mut mac);
            win.text(PAD, y + ty, b"MAC\0", FG_DIM);
            win.text(PAD + LABEL_W, y + ty, &mac, if ni.link_up != 0 { FG } else { FG_DIM });
            y += ROW_H;

            let (dtext, drgb) = dhcp_text(ni.dhcp_state);
            win.text(PAD, y + ty, b"DHCP\0", FG_DIM);
            win.text(PAD + LABEL_W, y + ty, dtext, drgb);
            y += ROW_H;

            addr_row(win, y + ty, b"Address\0", ni.ip);
            y += ROW_H;
            addr_row(win, y + ty, b"Netmask\0", ni.mask);
            y += ROW_H;
            addr_row(win, y + ty, b"Gateway\0", ni.gateway);
            y += ROW_H;
            addr_row(win, y + ty, b"DNS\0", ni.dns);
        }
    }

    // ── ARP cache ──
    let mut y = section(win, PAD, PAD + SECTION_H + 7 * ROW_H + GAP, w, b"ARP cache\0");
    let mut raw = [0u8; ARP_CAP];
    let n = arp_info(&mut raw).min(ARP_CAP);
    let text = &raw[..text_len(&raw[..n])];
    if text.is_empty() {
        win.text_fit(PAD, y + ty, b"The ARP cache could not be read.\0", w, RED);
    } else {
        win.text(PAD, y + ty, b"IP address\0", FG_DIM);
        win.text(PAD + 18 * FONT_W, y + ty, b"MAC address\0", FG_DIM);
        y += ROW_H;
        let mut shown = 0usize;
        let mut total = 0usize;
        for line in text.split(|&b| b == b'\n') {
            let Some((ip, mac)) = arp_entry(line) else { continue };
            total += 1;
            if shown < ARP_ROWS {
                win.text_fit(PAD, y + ty, ip, 16 * FONT_W, FG);
                win.text_fit(PAD + 18 * FONT_W, y + ty, mac, w - 18 * FONT_W, FG);
                y += ROW_H;
                shown += 1;
            }
        }
        if total == 0 {
            win.text_fit(PAD, y + ty, b"empty: nothing has been resolved yet\0", w, FG_DIM);
        } else if total > shown {
            // The last row is replaced by a count rather than silently cut.
            let mut more = [0u8; 40];
            let p = push_u32(&mut more, 0, (total - shown) as u32);
            push_str(&mut more, p, b" more entries not shown\0");
            win.fill(PAD, y - ROW_H, w, ROW_H, BG);
            win.text_fit(PAD, y - ROW_H + ty, &more, w, FG_DIM);
        }
    }

    status_bar(win, b"R: refresh now   Esc: close\0");
}

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    println!("rs-net: start");
    let Some(mut win) = Window::open(b"Network\0", WIN_W, WIN_H, 0) else {
        println!("rs-net: could not open a window");
        quit(1);
    };

    loop {
        draw(&win);
        win.flush();
        match win.wait(1000) {
            Some(Event::Close) | Some(Event::Key(KEY_ESC)) | Some(Event::Key(b'q')) => break,
            _ => {}                            // R, a timeout or anything else: just redraw
        }
    }

    win.close();
    println!("rs-net: exit");
}
