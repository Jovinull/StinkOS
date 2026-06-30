//! rs-files: StinkFS file browser for StinkOS.
//!
//! Lists all files on the StinkFS volume in a scrollable windowed list.
//! Arrow keys move the selection; Enter launches an ELF; Q exits.

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

const ROW_H:    i32 = 22;
const MAX_FILES: usize = 64;

fn content_top(cy: i32) -> i32 { cy + 8 }

/// Draw "NNNN B" or "NNN KB" into buf; NUL-terminates.
fn fmt_size(bytes: i32, buf: &mut [u8; 16]) {
    let n = if bytes < 0 { 0u32 } else { bytes as u32 };
    if n < 10000 {
        let mut tmp = [0u8; 12];
        let len = fmt_u32(n, &mut tmp);
        for i in 0..len { buf[i] = tmp[i]; }
        buf[len]     = b' ';
        buf[len + 1] = b'B';
        buf[len + 2] = 0;
    } else {
        let kb = n / 1024;
        let mut tmp = [0u8; 12];
        let len = fmt_u32(kb, &mut tmp);
        for i in 0..len { buf[i] = tmp[i]; }
        buf[len]     = b' ';
        buf[len + 1] = b'K';
        buf[len + 2] = b'B';
        buf[len + 3] = 0;
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    println!("rs-files: start");

    /* ── Collect file list ── */
    let raw_count = fcount();
    let count = if raw_count <= 0 { 0usize }
                else if raw_count as usize > MAX_FILES { MAX_FILES }
                else { raw_count as usize };

    let mut names = [[0u8; 16]; MAX_FILES];
    let mut sizes = [0i32; MAX_FILES];
    for i in 0..count {
        sizes[i] = finfo(i as i32, &mut names[i]);
    }

    /* ── Draw initial frame ── */
    fill(0, 0, SCREEN_W, SCREEN_H, BG);
    let content_y = window_frame(WIN_X, WIN_Y, WIN_W, WIN_H, b"File Browser\0");

    let list_top    = content_top(content_y);
    let list_bottom = WIN_Y + WIN_H - 24; /* leave room for footer */
    let visible     = ((list_bottom - list_top) / ROW_H) as usize;

    let mut selected  = 0usize;
    let mut scroll    = 0usize;
    let mut need_draw = true;
    let mut prev_key  = 0i32;

    loop {
        if need_draw {
            /* Content area background */
            fill(WIN_X + 1, content_y, WIN_W - 2, WIN_H - 34, SURFACE);

            if count == 0 {
                text16(WIN_X + 16, list_top, b"(no files)\0", FG_DIM);
            } else {
                for vis in 0..visible {
                    let idx = scroll + vis;
                    if idx >= count { break; }
                    let is_sel = idx == selected;
                    let row_y  = list_top + vis as i32 * ROW_H;

                    /* Row background */
                    let bg = if is_sel { SURFACE_ALT } else { SURFACE };
                    fill(WIN_X + 1, row_y, WIN_W - 2, ROW_H - 1, bg);

                    /* Accent bar on selected row */
                    if is_sel {
                        fill(WIN_X + 1, row_y, 3, ROW_H - 1, ACCENT);
                    }

                    /* Filename */
                    let name_rgb = if is_sel { ACCENT } else { FG };
                    text16(WIN_X + 12, row_y + (ROW_H - 8) / 2, &names[idx], name_rgb);

                    /* Size (right-aligned in a 100px column) */
                    let mut sbuf = [0u8; 16];
                    fmt_size(sizes[idx], &mut sbuf);
                    let slen  = nul_len(&sbuf) as i32;
                    let size_x = WIN_X + WIN_W - 8 - slen * 8;
                    text16(size_x, row_y + (ROW_H - 16) / 2, &sbuf, FG_DIM);
                }
            }

            /* Scrollbar indicator */
            if count > visible {
                let bar_h   = WIN_H - 34 - 24;
                let thumb_h = (bar_h * visible as i32) / count as i32;
                let thumb_y = content_y + (bar_h * scroll as i32) / count as i32;
                fill(WIN_X + WIN_W - 6, content_y, 4, bar_h, SURFACE_ALT);
                fill(WIN_X + WIN_W - 6, thumb_y, 4, thumb_h, BORDER);
            }

            /* Footer */
            let footer_y = WIN_Y + WIN_H - 20;
            fill(WIN_X + 1, footer_y - 4, WIN_W - 2, 1, BORDER);
            text16(WIN_X + 12, footer_y, b"Up/Down - navigate   Enter - launch   Q - quit\0", FG_DIM);

            need_draw = false;
        }

        let k = poll_key();
        if k != 0 && k != prev_key {
            if k == b'q' as i32 { break; }
            if k == KEY_UP && selected > 0 {
                selected -= 1;
                if selected < scroll { scroll = selected; }
                need_draw = true;
            }
            if k == KEY_DOWN && count > 0 && selected + 1 < count {
                selected += 1;
                if selected >= scroll + visible { scroll = selected + 1 - visible; }
                need_draw = true;
            }
            if (k == b'\n' as i32 || k == b'\r' as i32) && count > 0 {
                let pid = fork();
                if pid == 0 {
                    exec(&names[selected]);
                    quit(1);
                } else if pid > 0 {
                    wait_for(pid);
                    /* Child overwrote the screen — rebuild everything */
                    fill(0, 0, SCREEN_W, SCREEN_H, BG);
                    window_frame(WIN_X, WIN_Y, WIN_W, WIN_H, b"File Browser\0");
                    need_draw = true;
                }
            }
        }
        prev_key = k;

        sleep_ms(16);
    }

    println!("rs-files: exit");
}
