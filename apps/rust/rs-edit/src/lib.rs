//! rs-edit: text file editor for StinkOS.
//!
//! Opens a file picker on launch (non-ELF files only, plus a New-File option).
//! Navigate with arrows; type to insert; Backspace to delete; Ctrl+S to save;
//! Esc or Ctrl+Q to discard and quit.
//!
//! References:
//! - Two-pane file-picker layout: Serenity FileManager MainWidget.cpp
//! - Cursor / viewport model: simplified gap-buffer without gap (flat array,
//!   O(n) insert/delete is fine for the 4 KB StinkFS file cap)

#![no_std]
#![no_main]

use libstink::println;
use libui::*;

// ── Layout ────────────────────────────────────────────────────────────────────

const SCREEN_W: i32 = 1024;
const SCREEN_H: i32 = 768;
const WIN_X:    i32 = 40;
const WIN_Y:    i32 = 30;
const WIN_W:    i32 = SCREEN_W - 80;
const WIN_H:    i32 = SCREEN_H - 60;

// Titlebar height is 33px (from draw_window_frame) + 1px separator = 34
const CONTENT_TOP: i32 = WIN_Y + 34;
const CONTENT_BOT: i32 = WIN_Y + WIN_H - 22; // leave 22px for status bar
const TEXT_X:      i32 = WIN_X + 12;
const TEXT_W:      i32 = WIN_W - 24;
const LINE_H:      i32 = 18;
const CHARS_PER_LINE: usize = (TEXT_W / 8) as usize;
const VISIBLE_LINES:  usize = ((CONTENT_BOT - CONTENT_TOP) / LINE_H) as usize;

// ── Ctrl key codes ────────────────────────────────────────────────────────────

const CTRL_S:   i32 = 19; // Ctrl+S
const CTRL_Q:   i32 = 17; // Ctrl+Q
const CTRL_Z:   i32 = 26; // Ctrl+Z = undo
const KEY_ESC:  i32 = 27;
const KEY_BS:   i32 = 8;
const KEY_DEL:  i32 = 127;

// ── Undo stack ────────────────────────────────────────────────────────────────

const UNDO_CAP: usize = 128;

#[derive(Clone, Copy)]
struct UndoEntry {
    kind: u8,   // 0 = insert (undo: delete at pos), 1 = delete (undo: insert ch at pos)
    pos:  u16,
    ch:   u8,
}

struct UndoStack {
    buf:   [UndoEntry; UNDO_CAP],
    head:  usize, // next write slot
    count: usize, // valid entries
}

impl UndoStack {
    const fn new() -> Self {
        UndoStack {
            buf:   [UndoEntry { kind: 0, pos: 0, ch: 0 }; UNDO_CAP],
            head:  0,
            count: 0,
        }
    }
    fn push(&mut self, e: UndoEntry) {
        self.buf[self.head] = e;
        self.head = (self.head + 1) % UNDO_CAP;
        if self.count < UNDO_CAP { self.count += 1; }
    }
    fn pop(&mut self) -> Option<UndoEntry> {
        if self.count == 0 { return None; }
        self.count -= 1;
        self.head = if self.head == 0 { UNDO_CAP - 1 } else { self.head - 1 };
        Some(self.buf[self.head])
    }
}

// ── Buffer ────────────────────────────────────────────────────────────────────

const BUF_CAP: usize = 4096;

struct Editor {
    buf:     [u8; BUF_CAP],
    len:     usize,
    cursor:  usize,
    vp_line: usize,
    dirty:   bool,
    undo:    UndoStack,
}

impl Editor {
    const fn new() -> Self {
        Editor {
            buf: [0u8; BUF_CAP], len: 0, cursor: 0,
            vp_line: 0, dirty: false,
            undo: UndoStack::new(),
        }
    }

    /// Load bytes into buffer (truncates to BUF_CAP).
    fn load(&mut self, src: &[u8], n: usize) {
        let n = n.min(BUF_CAP);
        for i in 0..n { self.buf[i] = src[i]; }
        self.len    = n;
        self.cursor = 0;
        self.vp_line = 0;
        self.dirty  = false;
    }

    fn insert_raw(&mut self, c: u8) {
        if self.len >= BUF_CAP { return; }
        let pos = self.cursor;
        let mut i = self.len;
        while i > pos { self.buf[i] = self.buf[i - 1]; i -= 1; }
        self.buf[pos] = c;
        self.len    += 1;
        self.cursor += 1;
        self.dirty   = true;
    }

    fn backspace_raw(&mut self) {
        if self.cursor == 0 { return; }
        self.cursor -= 1;
        let pos = self.cursor;
        for i in pos..self.len - 1 { self.buf[i] = self.buf[i + 1]; }
        self.len   -= 1;
        self.dirty  = true;
    }

    fn insert(&mut self, c: u8) {
        self.undo.push(UndoEntry { kind: 0, pos: self.cursor as u16, ch: c });
        self.insert_raw(c);
    }

    fn backspace(&mut self) {
        if self.cursor == 0 { return; }
        let ch = self.buf[self.cursor - 1];
        self.undo.push(UndoEntry { kind: 1, pos: (self.cursor - 1) as u16, ch });
        self.backspace_raw();
    }

    fn undo(&mut self) {
        let Some(e) = self.undo.pop() else { return };
        if e.kind == 0 {
            // Was an insert at pos → undo: set cursor to pos+1, delete
            self.cursor = (e.pos as usize + 1).min(self.len);
            self.backspace_raw();
        } else {
            // Was a delete of ch at pos → undo: set cursor to pos, insert
            self.cursor = (e.pos as usize).min(self.len);
            self.insert_raw(e.ch);
            // After raw insert cursor is at pos+1; move back so user sees it right
        }
    }

    /// (line, col) of the cursor.
    fn cursor_pos(&self) -> (usize, usize) {
        let mut line = 0usize;
        let mut col  = 0usize;
        for i in 0..self.cursor {
            if self.buf[i] == b'\n' { line += 1; col = 0; }
            else                    { col  += 1; }
        }
        (line, col)
    }

    /// Total number of lines (number of '\n' + 1).
    fn total_lines(&self) -> usize {
        let mut n = 1usize;
        for i in 0..self.len { if self.buf[i] == b'\n' { n += 1; } }
        n
    }

    /// Move cursor left one byte.
    fn move_left(&mut self) {
        if self.cursor > 0 { self.cursor -= 1; }
    }

    /// Move cursor right one byte.
    fn move_right(&mut self) {
        if self.cursor < self.len { self.cursor += 1; }
    }

    /// Move cursor up one line, keeping column.
    fn move_up(&mut self) {
        let (line, col) = self.cursor_pos();
        if line == 0 { self.cursor = 0; return; }
        // Find start of previous line
        let target_line = line - 1;
        self.cursor = self.line_start(target_line) + col.min(self.line_len(target_line));
    }

    /// Move cursor down one line, keeping column.
    fn move_down(&mut self) {
        let (line, col) = self.cursor_pos();
        let total = self.total_lines();
        if line + 1 >= total { self.cursor = self.len; return; }
        let nxt = self.line_start(line + 1);
        let nxt_len = self.line_len(line + 1);
        self.cursor = nxt + col.min(nxt_len);
    }

    /// Byte offset of the start of the given line number.
    fn line_start(&self, target: usize) -> usize {
        if target == 0 { return 0; }
        let mut line = 0usize;
        for i in 0..self.len {
            if self.buf[i] == b'\n' {
                line += 1;
                if line == target { return i + 1; }
            }
        }
        self.len
    }

    /// Number of bytes on the given line (not counting the '\n').
    fn line_len(&self, target: usize) -> usize {
        let start = self.line_start(target);
        let mut n = 0usize;
        let mut i = start;
        while i < self.len && self.buf[i] != b'\n' { n += 1; i += 1; }
        n
    }

    /// Scroll viewport so cursor is visible.
    fn scroll_to_cursor(&mut self) {
        let (line, _) = self.cursor_pos();
        if line < self.vp_line {
            self.vp_line = line;
        } else if line >= self.vp_line + VISIBLE_LINES {
            self.vp_line = line + 1 - VISIBLE_LINES;
        }
    }
}

// ── File picker ───────────────────────────────────────────────────────────────

const PICK_MAX: usize = 48;

fn is_elf_name(name: &[u8; 16]) -> bool {
    let len = nul_len(name);
    len >= 4 && name[len - 4] == b'.'
             && (name[len - 3] == b'E' || name[len - 3] == b'e')
             && (name[len - 2] == b'L' || name[len - 2] == b'l')
             && (name[len - 1] == b'F' || name[len - 1] == b'f')
}

enum PickResult {
    Open([u8; 16]),  // filename to open
    New,             // empty new file
    Cancel,
}

fn file_picker() -> PickResult {
    let raw = fcount();
    let total = if raw <= 0 { 0usize } else { raw as usize };

    // Collect non-ELF files
    let mut names   = [[0u8; 16]; PICK_MAX];
    let mut count   = 0usize;
    for i in 0..total {
        let mut n = [0u8; 16];
        finfo(i as i32, &mut n);
        if !is_elf_name(&n) && count < PICK_MAX {
            names[count] = n;
            count += 1;
        }
    }

    fill(0, 0, SCREEN_W, SCREEN_H, BG);
    let content_y = window_frame(WIN_X, WIN_Y, WIN_W, WIN_H, b"rs-edit -- Open File\0");

    let row_h  = 22i32;
    let list_y = content_y + 8;
    let vis    = ((CONTENT_BOT - list_y) / row_h) as usize;

    let mut sel    = 0usize;
    let mut scroll = 0usize;
    let mut prev_k = 0i32;

    // Footer hint
    let footer_y = WIN_Y + WIN_H - 20;
    fill(WIN_X + 1, footer_y - 3, WIN_W - 2, 1, BORDER);
    text16(WIN_X + 12, footer_y, b"Up/Down - select   N - new file   Enter - open   Esc - cancel\0", FG_DIM);

    let mut redraw = true;
    loop {
        if redraw {
            fill(WIN_X + 1, list_y - 2, WIN_W - 2, CONTENT_BOT - list_y + 2, SURFACE);

            if count == 0 {
                text16(WIN_X + 16, list_y, b"(no text files)\0", FG_DIM);
            } else {
                for v in 0..vis {
                    let idx = scroll + v;
                    if idx >= count { break; }
                    let ry    = list_y + v as i32 * row_h;
                    let is_s  = idx == sel;
                    fill(WIN_X + 1, ry, WIN_W - 2, row_h - 1, if is_s { SURFACE_ALT } else { SURFACE });
                    if is_s { fill(WIN_X + 1, ry, 3, row_h - 1, ACCENT); }
                    text16(WIN_X + 12, ry + (row_h - 16) / 2, &names[idx],
                         if is_s { ACCENT } else { FG });
                }
            }
            redraw = false;
        }

        let k = poll_key();
        if k != 0 && k != prev_k {
            match k {
                k if k == KEY_ESC || k == CTRL_Q => return PickResult::Cancel,
                k if k == b'n' as i32 || k == b'N' as i32 => return PickResult::New,
                k if k == KEY_UP => {
                    if sel > 0 { sel -= 1; if sel < scroll { scroll = sel; } redraw = true; }
                }
                k if k == KEY_DOWN => {
                    if count > 0 && sel + 1 < count {
                        sel += 1;
                        if sel >= scroll + vis { scroll = sel + 1 - vis; }
                        redraw = true;
                    }
                }
                k if k == b'\n' as i32 || k == b'\r' as i32 => {
                    if count > 0 { return PickResult::Open(names[sel]); }
                }
                _ => {}
            }
        }
        prev_k = k;
        sleep_ms(16);
    }
}

// ── Rendering ─────────────────────────────────────────────────────────────────

fn render(ed: &Editor, fname: &[u8; 16]) {
    fill(0, 0, SCREEN_W, SCREEN_H, BG);

    // Build title: "rs-edit -- FILENAME[*]"
    let mut title = [0u8; 40];
    let prefix = b"rs-edit -- ";
    for i in 0..prefix.len() { title[i] = prefix[i]; }
    let mut ti = prefix.len();
    let fl = nul_len(fname);
    for i in 0..fl.min(16) { title[ti] = fname[i]; ti += 1; }
    if ed.dirty { title[ti] = b'*'; ti += 1; }
    title[ti] = 0;

    window_frame(WIN_X, WIN_Y, WIN_W, WIN_H, &title);

    // Line number gutter background
    fill(WIN_X + 1, CONTENT_TOP, 36, WIN_H - 34, 0x0a0f14);
    fill(WIN_X + 37, CONTENT_TOP, 1, WIN_H - 34, BORDER);

    let text_x = WIN_X + 42;
    let (cur_line, cur_col) = ed.cursor_pos();

    // Render visible lines
    for v in 0..VISIBLE_LINES {
        let line_num = ed.vp_line + v;
        let row_y    = CONTENT_TOP + v as i32 * LINE_H;
        if row_y + LINE_H > CONTENT_BOT { break; }

        // Background — highlight current line
        let row_bg = if line_num == cur_line { 0x161f2e } else { 0x0d1117 };
        fill(WIN_X + 38, row_y, WIN_W - 39, LINE_H, row_bg);

        // Line number
        let mut lnbuf = [0u8; 12];
        fmt_u32((line_num + 1) as u32, &mut lnbuf);
        let lnw = nul_len(&lnbuf) as i32;
        text16(WIN_X + 4 + (26 - lnw * 8), row_y + (LINE_H - 16) / 2, &lnbuf, FG_DIM);

        // Text content
        let ls   = ed.line_start(line_num);
        let ll   = ed.line_len(line_num);
        let disp = ll.min(CHARS_PER_LINE);

        if disp > 0 {
            let mut lbuf = [0u8; 128];
            for i in 0..disp { lbuf[i] = ed.buf[ls + i]; }
            lbuf[disp] = 0;
            text16(text_x, row_y + (LINE_H - 16) / 2, &lbuf, FG);
        }

        // Cursor
        if line_num == cur_line {
            let cx = text_x + cur_col.min(CHARS_PER_LINE) as i32 * 8;
            let cy = row_y + LINE_H - 2;
            fill(cx, cy, 8, 2, ACCENT);
        }
    }

    // Status bar
    let st_y = WIN_Y + WIN_H - 20;
    fill(WIN_X + 1, st_y - 3, WIN_W - 2, 1, BORDER);
    fill(WIN_X + 1, st_y - 2, WIN_W - 2, 22, 0x090e14);

    // Position indicator
    let mut posbuf = [0u8; 24];
    let mut pi = 0usize;
    let (cl, co) = (cur_line + 1, cur_col + 1);
    let mut tmp = [0u8; 12];
    let n = fmt_u32(cl as u32, &mut tmp);
    for i in 0..n { posbuf[pi] = tmp[i]; pi += 1; }
    posbuf[pi] = b':'; pi += 1;
    let n = fmt_u32(co as u32, &mut tmp);
    for i in 0..n { posbuf[pi] = tmp[i]; pi += 1; }
    posbuf[pi] = 0;

    text16(WIN_X + 12, st_y, &posbuf, FG_DIM);
    text16(WIN_X + 80, st_y, b"Ctrl+S save  Ctrl+Z undo  Ctrl+Q quit\0", FG_DIM);
}

// ── Main ──────────────────────────────────────────────────────────────────────

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    println!("rs-edit: start");

    let mut fname = [0u8; 16];
    let mut ed    = Editor::new();

    match file_picker() {
        PickResult::Cancel => {
            println!("rs-edit: cancelled");
            return;
        }
        PickResult::New => {
            // Empty editor with a default filename
            let dflt = b"NOTES.TXT\0";
            for i in 0..dflt.len() { fname[i] = dflt[i]; }
        }
        PickResult::Open(name) => {
            fname = name;
            let mut raw = [0u8; BUF_CAP];
            // sys_fread is available through libstink (extern "C" in libui)
            let n = fread(&fname, &mut raw);
            if n > 0 { ed.load(&raw, n as usize); }
        }
    }

    let mut prev_k = 0i32;

    loop {
        ed.scroll_to_cursor();
        render(&ed, &fname);

        let k = poll_key();
        if k != 0 && k != prev_k {
            match k {
                k if k == CTRL_Q || k == KEY_ESC => break,
                k if k == CTRL_S => {
                    fwrite(&fname, &ed.buf[..ed.len]);
                    ed.dirty = false;
                }
                k if k == CTRL_Z => ed.undo(),
                k if k == KEY_UP    => ed.move_up(),
                k if k == KEY_DOWN  => ed.move_down(),
                k if k == KEY_LEFT  => ed.move_left(),
                k if k == KEY_RIGHT => ed.move_right(),
                k if k == KEY_BS || k == KEY_DEL => ed.backspace(),
                k if k == b'\n' as i32 || k == b'\r' as i32 => ed.insert(b'\n'),
                k if k >= 32 && k < 127 => ed.insert(k as u8),
                _ => {}
            }
        }
        prev_k = k;
        sleep_ms(16);
    }

    println!("rs-edit: exit");
}
