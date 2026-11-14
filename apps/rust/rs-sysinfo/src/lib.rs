//! rs-sysinfo: uptime, wall clock, memory, file count and the process table.
//!
//! Everything on screen is read from the kernel once a second: the uptime
//! from the tick counter, the time from the RTC, memory from the frame
//! allocator, and the process table from the kernel's text snapshot, which
//! is split into columns here. Esc, Q or
//! the close box end it.

#![no_std]
#![no_main]

use libstink::println;
use libui::*;

/// The PIT ticks at 100 Hz.
const TICKS_PER_SEC: u32 = 100;

/// What `section` takes: caption, rule, spacing.
const SECTION_H: i32 = FONT_H + 3 + GAP;

/// Process rows the window has room for (the kernel's table holds 16).
const MAX_ROWS: usize = 16;

// Process table columns, as pixel offsets from the table's left edge.
// Numbers are right-aligned to the end of their column.
const X_PID_END:    i32 = 4 * FONT_W;
const X_STATE:      i32 = 6 * FONT_W;
const X_PRIO_END:   i32 = 16 * FONT_W;
const X_PARENT_END: i32 = 24 * FONT_W;
const X_NAME:       i32 = 26 * FONT_W;
const NAME_W:       i32 = 15 * FONT_W;

const WIN_W: i32 = 2 * PAD + X_NAME + NAME_W;
const WIN_H: i32 = PAD + SECTION_H + 4 * ROW_H            // System: uptime, time, memory, files
                 + GAP + SECTION_H + ROW_H                // Processes: column titles
                 + MAX_ROWS as i32 * ROW_H + GAP + STATUS_H;

// ── Process table text ──────────────────────────────────────────────────────

/// One row of the process table. The fields borrow from the snapshot text;
/// a field the line did not have is empty.
struct ProcRow<'a> {
    pid:    &'a [u8],
    state:  &'a [u8],
    prio:   &'a [u8],
    parent: &'a [u8],
    name:   &'a [u8],
}

static TITLES: ProcRow<'static> = ProcRow {
    pid: b"PID", state: b"STATE", prio: b"PRIO", parent: b"PARENT", name: b"NAME",
};

fn is_blank(b: u8) -> bool { b <= b' ' }

/// `s` without leading and trailing blanks.
fn trim(s: &[u8]) -> &[u8] {
    let start = s.iter().position(|&b| !is_blank(b)).unwrap_or(s.len());
    let end = s.iter().rposition(|&b| !is_blank(b)).map_or(start, |i| i + 1);
    &s[start..end]
}

/// Take the next blank-separated word off the front of `rest`.
fn word<'a>(rest: &mut &'a [u8]) -> &'a [u8] {
    let s = *rest;
    let start = s.iter().position(|&b| !is_blank(b)).unwrap_or(s.len());
    let s = &s[start..];
    let end = s.iter().position(|&b| is_blank(b)).unwrap_or(s.len());
    *rest = &s[end..];
    &s[..end]
}

/// A snapshot line as a process row: "PID STATE PRIO PARENT NAME", blank
/// separated. Only a line that starts with a number is one, which also
/// drops the kernel's own title line.
fn parse_row(line: &[u8]) -> Option<ProcRow<'_>> {
    let mut rest = line;
    let pid = word(&mut rest);
    if pid.is_empty() || !pid.iter().all(|b| b.is_ascii_digit()) {
        return None;
    }
    let state = word(&mut rest);
    let prio = word(&mut rest);
    let parent = word(&mut rest);
    Some(ProcRow { pid, state, prio, parent, name: trim(rest) })
}

fn proc_rows<'a>(text: &'a [u8]) -> impl Iterator<Item = ProcRow<'a>> + 'a {
    text.split(|&b| b == b'\n').filter_map(parse_row)
}

// ── Drawing ─────────────────────────────────────────────────────────────────

/// At most the first `n` bytes of `s`.
fn cut(s: &[u8], n: usize) -> &[u8] {
    &s[..s.len().min(n)]
}

fn table_row(win: &Window, x: i32, y: i32, r: &ProcRow, rgb: u32) {
    let ty = y + (ROW_H - FONT_H) / 2;
    win.text_right(x + X_PID_END, ty, cut(r.pid, 4), rgb);
    win.text(x + X_STATE, ty, cut(r.state, 5), rgb);
    win.text_right(x + X_PRIO_END, ty, cut(r.prio, 4), rgb);
    win.text_right(x + X_PARENT_END, ty, cut(r.parent, 6), rgb);
    win.text_fit(x + X_NAME, ty, r.name, NAME_W, rgb);
}

/// "HH:MM:SS\0", with as many hour digits as the uptime needs.
fn fmt_uptime(secs: u32, out: &mut [u8; 16]) {
    let mut ms = [0u8; 9];
    fmt_hhmmss(0, (secs / 60) % 60, secs % 60, &mut ms);
    let hours = secs / 3600;
    let mut p = 0;
    if hours < 10 {
        p = push_str(out, p, b"0\0");
    }
    p = push_u32(out, p, hours);
    push_str(out, p, &ms[2..]);                    // ":MM:SS"
}

/// `procs` is the process snapshot text; empty when it could not be read.
fn draw(win: &Window, uptime: u32, clock: Option<&RtcTime>, procs: &[u8]) {
    win.clear(BG);
    let x = PAD;
    let w = win.w() - 2 * PAD;
    let right = x + 30 * FONT_W;

    let mut y = section(win, x, PAD, w, b"System\0");
    let mut up = [0u8; 16];
    fmt_uptime(uptime, &mut up);
    field(win, x, y, right, b"Uptime\0", &up, FG);
    y += ROW_H;
    match clock {
        Some(t) => {
            let mut buf = [0u8; 9];
            fmt_hhmmss(t.hour, t.minute, t.second, &mut buf);
            field(win, x, y, right, b"Time\0", &buf, FG);
        }
        None => field(win, x, y, right, b"Time\0", b"unavailable\0", FG_DIM),
    }
    y += ROW_H;

    // Physical memory the kernel manages, in 4 KiB pages.
    let mut mem = [0u8; 40];
    match meminfo() {
        Some(m) => {
            let used_kib = (m.total_pages - m.free_pages.min(m.total_pages)) * 4;
            let mut p = push_u32(&mut mem, 0, used_kib / 1024);
            p = push_str(&mut mem, p, b" of \0");
            p = push_u32(&mut mem, p, m.total_pages * 4 / 1024);
            push_str(&mut mem, p, b" MiB used\0");
            field(win, x, y, right, b"Memory\0", &mem, FG);
        }
        None => field(win, x, y, right, b"Memory\0", b"unavailable\0", FG_DIM),
    }
    y += ROW_H;

    let files = fcount();
    let mut fbuf = [0u8; 12];
    if files >= 0 {
        fmt_u32(files as u32, &mut fbuf);
        field(win, x, y, right, b"Files\0", &fbuf, FG);
    } else {
        field(win, x, y, right, b"Files\0", b"unavailable\0", FG_DIM);
    }
    y += ROW_H;

    y = section(win, x, y + GAP, w, b"Processes\0");
    if procs.is_empty() {
        win.text_fit(x, y, b"The process table could not be read.\0", w, RED);
    } else {
        table_row(win, x, y, &TITLES, FG_DIM);
        y += ROW_H;
        // When there are more rows than room, the last line says how many
        // are missing instead of showing one of them.
        let total = proc_rows(procs).count();
        let shown = if total > MAX_ROWS { MAX_ROWS - 1 } else { total };
        for row in proc_rows(procs).take(shown) {
            table_row(win, x, y, &row, FG);
            y += ROW_H;
        }
        if total > shown {
            let mut more = [0u8; 32];
            let p = push_u32(&mut more, 0, (total - shown) as u32);
            push_str(&mut more, p, b" more not shown\0");
            win.text_fit(x, y + (ROW_H - FONT_H) / 2, &more, w, FG_DIM);
        }
    }

    status_bar(win, b"Esc: close\0");
}

// ── Main ────────────────────────────────────────────────────────────────────

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    println!("rs-sysinfo: start");
    let Some(mut win) = Window::open(b"System Info\0", WIN_W, WIN_H, 0) else {
        println!("rs-sysinfo: could not open a window");
        quit(1);
    };

    let mut procs = [0u8; 2048];
    let mut shown_sec: Option<u32> = None;       // uptime second on screen

    loop {
        // Read and redraw once per uptime second; input alone changes nothing.
        let sec = ticks() / TICKS_PER_SEC;
        if shown_sec != Some(sec) {
            let n = proc_info(&mut procs).min(procs.len());
            let n = text_len(&procs[..n]);
            let mut rtc = RtcTime { year: 0, month: 0, day: 0, hour: 0, minute: 0, second: 0 };
            let clock = if read_clock(&mut rtc) { Some(&rtc) } else { None };
            draw(&win, sec, clock, &procs[..n]);
            win.flush();
            shown_sec = Some(sec);
        }

        // Sleep until the uptime changes, or until input arrives.
        let left = TICKS_PER_SEC - ticks() % TICKS_PER_SEC;
        match win.wait(left * 10) {
            Some(Event::Close) | Some(Event::Key(KEY_ESC | b'q' | b'Q')) => break,
            _ => {}
        }
    }

    win.close();
    println!("rs-sysinfo: exit");
}
