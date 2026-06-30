//! rs-hex: hexadecimal file viewer for StinkOS.
//!
//! Browses StinkFS files, reads selected file into a 4KB buffer,
//! and shows classic hex dump: offset | 16 hex bytes | ASCII sidebar.
//!
//! Controls:
//!   Left/Right — previous/next file
//!   Up/Down    — scroll dump by one row
//!   PgUp/PgDn  — scroll by one page (handled via KEY_UP/DOWN held)
//!   Q / Esc    — quit

#![no_std]
#![no_main]

use libstink::println;
use libui::*;

// ── Layout ────────────────────────────────────────────────────────────────────

const SCREEN_W: i32 = 1024;
const SCREEN_H: i32 = 768;
const WIN_X: i32 = 40;
const WIN_Y: i32 = 40;
const WIN_W: i32 = SCREEN_W - 80;
const WIN_H: i32 = SCREEN_H - 80;

const ROW_H:   i32 = 18;
const BYTES_PER_ROW: usize = 16;
const BUF_CAP: usize = 4096;
const MAX_FILES: usize = 64;

// hex char lookup
const HEX: &[u8; 16] = b"0123456789abcdef";

fn byte_to_hex(b: u8, out: &mut [u8; 2]) {
    out[0] = HEX[(b >> 4) as usize];
    out[1] = HEX[(b & 0xf) as usize];
}

// ── Rendering ─────────────────────────────────────────────────────────────────

fn draw_header(name: &[u8; 16], file_len: usize, file_idx: usize, file_count: usize) {
    let title_y = WIN_Y + 34 + 4;
    let cx = WIN_X + 8;

    // File name
    text16(cx, title_y, name, FG);

    // Size
    let mut sbuf = [0u8; 24];
    {
        let mut pos = 0usize;
        sbuf[pos] = b' '; pos += 1;
        sbuf[pos] = b'('; pos += 1;
        let mut tmp = [0u8; 12];
        let l = fmt_u32(file_len as u32, &mut tmp);
        for i in 0..l { sbuf[pos] = tmp[i]; pos += 1; }
        sbuf[pos] = b' '; pos += 1;
        sbuf[pos] = b'B'; pos += 1;
        sbuf[pos] = b')'; pos += 1;
        sbuf[pos] = 0;
    }
    text16(cx + 16 * 8, title_y, &sbuf, FG_DIM);

    // File index indicator
    let mut ibuf = [0u8; 16];
    {
        let mut pos = 0usize;
        let mut tmp = [0u8; 12];
        let l = fmt_u32(file_idx as u32 + 1, &mut tmp);
        for i in 0..l { ibuf[pos] = tmp[i]; pos += 1; }
        ibuf[pos] = b'/'; pos += 1;
        let l2 = fmt_u32(file_count as u32, &mut tmp);
        for i in 0..l2 { ibuf[pos] = tmp[i]; pos += 1; }
        ibuf[pos] = 0;
    }
    text16(WIN_X + WIN_W - 60, title_y, &ibuf, FG_DIM);

    // Separator
    fill(WIN_X + 1, title_y + 12, WIN_W - 2, 1, BORDER);
}

fn draw_dump(buf: &[u8], file_len: usize, scroll: usize, cx: i32, top_y: i32, bottom_y: i32) {
    let mut y = top_y;
    let mut row_off = scroll * BYTES_PER_ROW;

    while y + ROW_H <= bottom_y && row_off < file_len {
        // Offset
        let mut obuf = [0u8; 8];
        let mut tmp2 = [0u8; 2];
        byte_to_hex((row_off >> 24) as u8, &mut tmp2);
        obuf[0] = tmp2[0]; obuf[1] = tmp2[1];
        byte_to_hex((row_off >> 16) as u8, &mut tmp2);
        obuf[2] = tmp2[0]; obuf[3] = tmp2[1];
        byte_to_hex((row_off >> 8) as u8, &mut tmp2);
        obuf[4] = tmp2[0]; obuf[5] = tmp2[1];
        byte_to_hex(row_off as u8, &mut tmp2);
        obuf[6] = tmp2[0]; obuf[7] = tmp2[1];
        // Print as "0000 0010 " style (no NUL string - use draw manually)
        // Build full display line: "00000000  xx xx ... |ascii|"
        let mut line = [b' '; 78];
        line[0] = obuf[0]; line[1] = obuf[1]; line[2] = obuf[2]; line[3] = obuf[3];
        line[4] = obuf[4]; line[5] = obuf[5]; line[6] = obuf[6]; line[7] = obuf[7];
        line[8] = b' '; line[9] = b' ';

        let row_bytes = BYTES_PER_ROW.min(file_len - row_off);
        let mut lp = 10usize;
        for bi in 0..BYTES_PER_ROW {
            if bi < row_bytes {
                let b = buf[row_off + bi];
                byte_to_hex(b, &mut tmp2);
                line[lp] = tmp2[0]; lp += 1;
                line[lp] = tmp2[1]; lp += 1;
            } else {
                line[lp] = b' '; lp += 1;
                line[lp] = b' '; lp += 1;
            }
            line[lp] = b' '; lp += 1;
            if bi == 7 { line[lp] = b' '; lp += 1; }
        }
        // Separator
        line[lp] = b'|'; lp += 1;
        // ASCII
        for bi in 0..BYTES_PER_ROW {
            if bi < row_bytes {
                let b = buf[row_off + bi];
                line[lp] = if b >= 32 && b < 127 { b } else { b'.' };
            } else {
                line[lp] = b' ';
            }
            lp += 1;
        }
        line[lp] = b'|';

        // Render line char-by-char
        // Hex segment: chars 10..58 in accent color
        // ASCII segment: chars 60..76 in FG
        // Offset: chars 0..8 in FG_DIM
        let char_w = 8i32;

        // Offset
        for i in 0..8usize {
            let mut cb = [line[i], 0];
            text16(cx + i as i32 * char_w, y, &cb, FG_DIM);
            cb[0] = 0; // silence unused warning
        }
        // Spacer ": "
        {
            let cb = [b':', b' ', 0];
            text16(cx + 8 * char_w, y, &cb, BORDER);
        }
        // Hex bytes
        for i in 10..=(10 + BYTES_PER_ROW * 3) {
            if i >= lp { break; }
            let col = if i >= 10 && i <= 10 + BYTES_PER_ROW * 3 { 0x8899aa } else { FG };
            let mut cb = [line[i], 0u8];
            text16(cx + i as i32 * char_w, y, &cb, col);
        }
        // Pipe + ASCII
        let pipe_i = 10 + BYTES_PER_ROW * 3 + 2;
        for i in pipe_i..=pipe_i + BYTES_PER_ROW + 1 {
            if i >= line.len() { break; }
            let col = if line[i] == b'|' { BORDER } else { FG };
            let mut cb = [line[i], 0u8];
            text16(cx + i as i32 * char_w, y, &cb, col);
        }

        y += ROW_H;
        row_off += BYTES_PER_ROW;
    }
}

// ── Main ──────────────────────────────────────────────────────────────────────

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    println!("rs-hex: start");

    // Collect file list
    let raw_count = fcount();
    let file_count = if raw_count <= 0 { 0usize }
                     else if raw_count as usize > MAX_FILES { MAX_FILES }
                     else { raw_count as usize };

    if file_count == 0 {
        println!("rs-hex: no files");
        return;
    }

    let mut names = [[0u8; 16]; MAX_FILES];
    let mut fsizes = [0i32; MAX_FILES];
    for i in 0..file_count {
        fsizes[i] = finfo(i as i32, &mut names[i]);
    }

    let mut buf = [0u8; BUF_CAP];
    let mut file_idx = 0usize;
    let mut scroll = 0usize;
    let mut file_len = 0usize;
    let mut dirty = true;
    let mut prev_k = 0i32;

    // Load first file
    {
        let n = fread(&names[file_idx], &mut buf);
        file_len = if n > 0 { (n as usize).min(BUF_CAP) } else { 0 };
    }

    loop {
        if dirty {
            fill(0, 0, SCREEN_W, SCREEN_H, BG);
            window_frame(WIN_X, WIN_Y, WIN_W, WIN_H, b"Hex Viewer\0");

            let top_y = WIN_Y + 34 + 20;
            let bot_y = WIN_Y + WIN_H - 20;
            let cx    = WIN_X + 8;

            draw_header(&names[file_idx], file_len, file_idx, file_count);
            fill(WIN_X + 1, WIN_Y + 34, WIN_W - 2, WIN_H - 35, 0x08100a);
            draw_dump(&buf, file_len, scroll, cx, top_y, bot_y);

            // Footer
            let footer_y = WIN_Y + WIN_H - 20;
            fill(WIN_X + 1, footer_y - 4, WIN_W - 2, 1, BORDER);
            text16(cx, footer_y, b"Left/Right: file   Up/Down: scroll   Q: quit\0", FG_DIM);

            dirty = false;
        }

        let k = poll_key();
        if k != 0 && k != prev_k {
            let rows_total = if file_len == 0 { 0 } else { (file_len + BYTES_PER_ROW - 1) / BYTES_PER_ROW };
            let top_y = WIN_Y + 34 + 20;
            let bot_y = WIN_Y + WIN_H - 20;
            let visible_rows = ((bot_y - top_y) / ROW_H) as usize;

            match k {
                k if k == b'q' as i32 || k == 27 => break,
                k if k == KEY_UP => {
                    if scroll > 0 { scroll -= 1; dirty = true; }
                }
                k if k == KEY_DOWN => {
                    let max_scroll = if rows_total > visible_rows { rows_total - visible_rows } else { 0 };
                    if scroll < max_scroll { scroll += 1; dirty = true; }
                }
                k if k == KEY_LEFT => {
                    if file_idx > 0 {
                        file_idx -= 1;
                    } else {
                        file_idx = file_count - 1;
                    }
                    let n = fread(&names[file_idx], &mut buf);
                    file_len = if n > 0 { (n as usize).min(BUF_CAP) } else { 0 };
                    scroll = 0;
                    dirty = true;
                }
                k if k == KEY_RIGHT => {
                    file_idx = (file_idx + 1) % file_count;
                    let n = fread(&names[file_idx], &mut buf);
                    file_len = if n > 0 { (n as usize).min(BUF_CAP) } else { 0 };
                    scroll = 0;
                    dirty = true;
                }
                _ => {}
            }
        }
        prev_k = k;
        sleep_ms(16);
    }

    println!("rs-hex: exit");
}
