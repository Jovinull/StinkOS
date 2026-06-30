//! rs-taskman: process manager for StinkOS.
//!
//! Reads live process list via sys_proc_info, displays in a scrollable table,
//! and allows sending SIGKILL to selected processes via sys_kill (syscall 46).
//!
//! Controls:
//!   Arrow Up/Down — navigate process list
//!   K             — kill selected process
//!   R             — refresh list
//!   Q / Escape    — quit

#![no_std]
#![no_main]

use libstink::println;
use libui::*;

// ── Layout ────────────────────────────────────────────────────────────────────

const SCREEN_W: i32 = 1024;
const SCREEN_H: i32 = 768;
const WIN_W: i32 = 800;
const WIN_H: i32 = 600;
const WIN_X: i32 = (SCREEN_W - WIN_W) / 2;
const WIN_Y: i32 = (SCREEN_H - WIN_H) / 2;

const CONTENT_X: i32 = WIN_X + 8;
const CONTENT_Y: i32 = WIN_Y + 34;
const ROW_H: i32 = 14;
const MAX_PROCS: usize = 32;

// Visible rows = (content area height - header - hint bar) / ROW_H
const VISIBLE: usize = ((WIN_H - 34 - 14 - 20 - 8) / ROW_H) as usize;

// ── Process parsing ───────────────────────────────────────────────────────────

struct ProcEntry {
    pid:  i32,
    line: [u8; 48],
    len:  usize,
}

fn parse_dec(buf: &[u8], pos: &mut usize) -> i32 {
    // Skip leading spaces
    while *pos < buf.len() && buf[*pos] == b' ' { *pos += 1; }
    let mut n = 0i32;
    while *pos < buf.len() && buf[*pos] >= b'0' && buf[*pos] <= b'9' {
        n = n * 10 + (buf[*pos] - b'0') as i32;
        *pos += 1;
    }
    n
}

fn parse_procs(raw: &[u8], procs: &mut [ProcEntry; MAX_PROCS]) -> usize {
    let mut count = 0usize;
    let mut i = 0usize;
    // Skip header line
    while i < raw.len() && raw[i] != b'\n' { i += 1; }
    if i < raw.len() { i += 1; }

    while i < raw.len() && count < MAX_PROCS {
        // Find end of this line
        let line_start = i;
        while i < raw.len() && raw[i] != b'\n' { i += 1; }
        let line_end = i;
        if i < raw.len() { i += 1; }
        let line = &raw[line_start..line_end];
        if line.is_empty() { continue; }

        // Parse PID from start of line
        let mut pos = 0usize;
        let pid = parse_dec(line, &mut pos);
        if pid <= 0 { continue; }

        let e = &mut procs[count];
        e.pid = pid;
        let copy_len = line.len().min(e.line.len() - 1);
        e.line[..copy_len].copy_from_slice(&line[..copy_len]);
        e.line[copy_len] = 0;
        e.len = copy_len;
        count += 1;
    }
    count
}

// ── Rendering ─────────────────────────────────────────────────────────────────

fn draw(procs: &[ProcEntry; MAX_PROCS], count: usize, sel: usize, my_pid: i32) {
    fill(0, 0, SCREEN_W, SCREEN_H, BG);
    window_frame(WIN_X, WIN_Y, WIN_W, WIN_H, b"Task Manager\0");

    // Column header
    let hy = CONTENT_Y;
    fill(CONTENT_X, hy, WIN_W - 16, ROW_H + 2, SURFACE);
    text(CONTENT_X + 4, hy + 3, b"PID  STATE PRIO PARENT NAME\0", FG_DIM);
    fill(CONTENT_X, hy + ROW_H + 2, WIN_W - 16, 1, BORDER);

    // Process rows
    let list_y = hy + ROW_H + 4;
    let visible_end = (sel / VISIBLE) * VISIBLE + VISIBLE;
    let scroll_off = if sel >= VISIBLE { (sel / VISIBLE) * VISIBLE } else { 0 };

    for vi in 0..VISIBLE {
        let pi = scroll_off + vi;
        if pi >= count { break; }

        let ry = list_y + vi as i32 * ROW_H;
        let is_sel = pi == sel;
        let is_me  = procs[pi].pid == my_pid;

        let bg = if is_sel { ACCENT } else { BG };
        let fg = if is_sel { BG }
                 else if is_me { YELLOW }
                 else { FG };

        if is_sel { fill(CONTENT_X, ry, WIN_W - 16, ROW_H, bg); }

        let mut tmp = [0u8; 50];
        let l = procs[pi].len.min(49);
        tmp[..l].copy_from_slice(&procs[pi].line[..l]);
        tmp[l] = 0;
        text(CONTENT_X + 4, ry + 3, &tmp, fg);
    }

    // Scroll indicator
    if count > VISIBLE {
        let bar_h = WIN_H - 34 - 20 - 8;
        let bar_x = WIN_X + WIN_W - 12;
        fill(bar_x, list_y, 4, bar_h, SURFACE);
        let thumb_h = bar_h * VISIBLE as i32 / count as i32;
        let thumb_y = list_y + bar_h * scroll_off as i32 / count as i32;
        fill(bar_x, thumb_y, 4, thumb_h.max(8), BORDER);
    }

    // Hint bar
    let hint_y = WIN_Y + WIN_H - 18;
    fill(WIN_X + 8, hint_y - 2, WIN_W - 16, 1, BORDER);
    text(CONTENT_X, hint_y + 2, b"K kill  auto-refresh 2s  Q quit  (yellow=self)\0", FG_DIM);

    // Count
    let mut cbuf = [0u8; 12];
    let cn = fmt_u32(count as u32, &mut cbuf);
    let _ = cn;
    text(WIN_X + WIN_W - 80, hint_y + 2, b"procs: \0", FG_DIM);
    text(WIN_X + WIN_W - 80 + 56, hint_y + 2, &cbuf, FG);

    _ = visible_end;
}

// ── Main ──────────────────────────────────────────────────────────────────────

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    println!("rs-taskman: start");

    let mut procs = core::array::from_fn(|_| ProcEntry { pid: 0, line: [0u8; 48], len: 0 });
    let mut raw = [0u8; 2048];
    let my_pid = getpid();

    let mut count = 0usize;
    let mut sel   = 0usize;
    let mut dirty = true;
    let mut prev_k = 0i32;
    let mut last_refresh: u32 = 0;

    // Refresh immediately
    let n = proc_info(&mut raw);
    if n > 0 {
        count = parse_procs(&raw[..n as usize], &mut procs);
    }

    loop {
        // Auto-refresh every 2 seconds
        let now = ticks();
        if now.wrapping_sub(last_refresh) >= 2000 {
            let n2 = proc_info(&mut raw);
            if n2 > 0 {
                let new_count = parse_procs(&raw[..n2 as usize], &mut procs);
                if new_count != count {
                    count = new_count;
                    if sel >= count && count > 0 { sel = count - 1; }
                    dirty = true;
                }
            }
            last_refresh = now;
        }

        if dirty {
            draw(&procs, count, sel, my_pid);
            dirty = false;
        }

        let k = poll_key();
        if k != 0 && k != prev_k {
            match k {
                k if k == b'q' as i32 || k == 27 => break,
                k if k == KEY_UP => {
                    if sel > 0 { sel -= 1; dirty = true; }
                }
                k if k == KEY_DOWN => {
                    if sel + 1 < count { sel += 1; dirty = true; }
                }
                k if k == b'k' as i32 || k == b'K' as i32 => {
                    if count > 0 {
                        let target = procs[sel].pid;
                        if target != my_pid {
                            kill(target);
                            sleep_ms(100);
                            // Refresh
                            let n2 = proc_info(&mut raw);
                            if n2 > 0 {
                                count = parse_procs(&raw[..n2 as usize], &mut procs);
                                if sel >= count && count > 0 { sel = count - 1; }
                            }
                            dirty = true;
                        }
                    }
                }
                k if k == b'r' as i32 || k == b'R' as i32 => {
                    let n2 = proc_info(&mut raw);
                    if n2 > 0 {
                        count = parse_procs(&raw[..n2 as usize], &mut procs);
                        if sel >= count && count > 0 { sel = count - 1; }
                    }
                    dirty = true;
                }
                _ => {}
            }
        }
        prev_k = k;

        sleep_ms(50);
    }

    println!("rs-taskman: exit");
}
