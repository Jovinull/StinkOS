//! rs-edit: text file editor for StinkOS.
//!
//! Starts in a file picker listing every file that is not an .ELF (Up/Down,
//! Enter to open, N for a new file), unless the file browser handed over a
//! name through LAUNCH.TMP (see `take_launch_name`).
//!
//! Editing: type to insert, Enter for a new line, Backspace / Delete, arrows,
//! Home / End, PgUp / PgDn. Ctrl+S saves, Ctrl+Z undoes, Ctrl+C copies the
//! whole buffer to the clipboard, Ctrl+V pastes at the cursor. Esc, Ctrl+Q or
//! the close box quit WITHOUT saving; unsaved changes show as "[modified]" in
//! the status bar.
//!
//! The buffer holds `BUF_CAP` bytes. A file that does not fit is refused, so
//! a save can never write back less than was on disk. Bytes are kept exactly
//! as read; anything that is not printable ASCII is drawn as '.'.

#![no_std]
#![no_main]

use libstink::println;
use libui::*;

// ── Layout ──────────────────────────────────────────────────────────────────

const WIN_W: i32 = 800;
const WIN_H: i32 = 500;

const LINE_H:   i32 = FONT_H + 2;
/// Line-number gutter: four digits, because `BUF_CAP` bytes are at most
/// `BUF_CAP + 1` lines.
const GUTTER_W: i32 = 4 * FONT_W + 8;
const TEXT_X:   i32 = GUTTER_W + 5;
/// Most columns a row can show (the widest window is `MAX_W` pixels).
const ROW_BUF: usize = 128;

/// Where the picker's list starts: just under its `section` caption.
const LIST_TOP: i32 = PAD + FONT_H + 3 + GAP;
/// The picker shows file sizes only when a row is at least this wide.
const SIZE_MIN_W: i32 = 36 * FONT_W;

// ── Keys ────────────────────────────────────────────────────────────────────

const CTRL_C: u8 = ctrl(b'c');
const CTRL_Q: u8 = ctrl(b'q');
const CTRL_S: u8 = ctrl(b's');
const CTRL_V: u8 = ctrl(b'v');
const CTRL_Z: u8 = ctrl(b'z');
const KEY_DEL: u8 = 127;

// ── Status message ──────────────────────────────────────────────────────────

/// One line shown in the status bar in place of the key hints, until the
/// next key press.
struct Msg {
    text: [u8; 96],
    len:  usize,
    rgb:  u32,
}

impl Msg {
    const fn new() -> Msg { Msg { text: [0; 96], len: 0, rgb: FG } }

    fn clear(&mut self) {
        self.len = 0;
        self.text[0] = 0;
    }

    fn is_set(&self) -> bool { self.len > 0 }

    /// Start a new message in colour `rgb`; append to it with `s` and `n`.
    fn set(&mut self, rgb: u32) -> &mut Msg {
        self.clear();
        self.rgb = rgb;
        self
    }

    fn s(&mut self, s: &[u8]) -> &mut Msg {
        self.len = push_str(&mut self.text, self.len, s);
        self
    }

    fn n(&mut self, n: usize) -> &mut Msg {
        self.len = push_u32(&mut self.text, self.len, n as u32);
        self
    }
}

// ── Undo ────────────────────────────────────────────────────────────────────

const UNDO_CAP: usize = 128;

#[derive(Clone, Copy)]
enum Edit {
    /// `len` bytes were inserted at `pos` (one typed character, or a paste).
    Insert { pos: u16, len: u16 },
    /// Byte `ch` was removed from `pos`. `back` when Backspace did it, so
    /// the cursor belongs after the byte once it is restored.
    Delete { pos: u16, ch: u8, back: bool },
}

/// The last `UNDO_CAP` edits; older ones fall off the far end.
struct UndoStack {
    buf:   [Edit; UNDO_CAP],
    head:  usize, // next write slot
    count: usize, // valid entries
}

impl UndoStack {
    const fn new() -> UndoStack {
        UndoStack { buf: [Edit::Insert { pos: 0, len: 0 }; UNDO_CAP], head: 0, count: 0 }
    }

    fn clear(&mut self) {
        self.head = 0;
        self.count = 0;
    }

    fn push(&mut self, e: Edit) {
        self.buf[self.head] = e;
        self.head = (self.head + 1) % UNDO_CAP;
        if self.count < UNDO_CAP { self.count += 1; }
    }

    fn pop(&mut self) -> Option<Edit> {
        if self.count == 0 { return None; }
        self.count -= 1;
        self.head = if self.head == 0 { UNDO_CAP - 1 } else { self.head - 1 };
        Some(self.buf[self.head])
    }
}

// ── Buffer ──────────────────────────────────────────────────────────────────

/// Largest file the editor holds, in bytes. It equals the kernel clipboard's
/// capacity, so "copy all" always copies everything.
const BUF_CAP: usize = 4096;

// Undo entries store positions and lengths as u16.
const _: () = assert!(BUF_CAP <= u16::MAX as usize);

enum LoadError {
    /// The file is missing or the disk read failed.
    Read,
    /// The file has more than `BUF_CAP` bytes.
    TooLarge,
}

struct Editor {
    buf:      [u8; BUF_CAP],
    len:      usize,
    cursor:   usize,   // byte offset, 0..=len
    top:      usize,   // first visible line
    left:     usize,   // first visible column
    modified: bool,    // differs from what was last loaded or saved
    undo:     UndoStack,
}

impl Editor {
    const fn new() -> Editor {
        Editor {
            buf: [0u8; BUF_CAP], len: 0, cursor: 0, top: 0, left: 0,
            modified: false, undo: UndoStack::new(),
        }
    }

    /// Back to an empty, unmodified buffer.
    fn reset(&mut self) {
        self.len = 0;
        self.cursor = 0;
        self.top = 0;
        self.left = 0;
        self.modified = false;
        self.undo.clear();
    }

    /// Replace the buffer with file `name`. On error the buffer is left as
    /// it was. Reading into one byte more than the buffer holds is what
    /// tells a file of exactly `BUF_CAP` bytes from a larger one.
    fn load(&mut self, name: &[u8; 16]) -> Result<(), LoadError> {
        let mut raw = [0u8; BUF_CAP + 1];
        let n = fread(name, &mut raw);
        if n < 0 { return Err(LoadError::Read); }
        let n = n as usize;
        if n > BUF_CAP { return Err(LoadError::TooLarge); }
        self.reset();
        self.buf[..n].copy_from_slice(&raw[..n]);
        self.len = n;
        Ok(())
    }

    // ── Lines ──

    /// Offset of the first byte of the line containing `pos`.
    fn line_start(&self, pos: usize) -> usize {
        let mut i = pos.min(self.len);
        while i > 0 && self.buf[i - 1] != b'\n' { i -= 1; }
        i
    }

    /// Offset of the '\n' that ends the line containing `pos` (or `len`).
    fn line_end(&self, pos: usize) -> usize {
        let mut i = pos.min(self.len);
        while i < self.len && self.buf[i] != b'\n' { i += 1; }
        i
    }

    /// Offset where line `n` (0-based) starts, if the buffer has that line.
    fn line_offset(&self, n: usize) -> Option<usize> {
        if n == 0 { return Some(0); }
        let mut seen = 0usize;
        for i in 0..self.len {
            if self.buf[i] == b'\n' {
                seen += 1;
                if seen == n { return Some(i + 1); }
            }
        }
        None
    }

    /// (line, column) of the cursor, both 0-based.
    fn cursor_line_col(&self) -> (usize, usize) {
        let line = self.buf[..self.cursor].iter().filter(|&&b| b == b'\n').count();
        (line, self.cursor - self.line_start(self.cursor))
    }

    // ── Editing ──

    fn raw_insert(&mut self, pos: usize, bytes: &[u8]) -> bool {
        let n = bytes.len();
        if pos > self.len || n > BUF_CAP - self.len { return false; }
        self.buf.copy_within(pos..self.len, pos + n);
        self.buf[pos..pos + n].copy_from_slice(bytes);
        self.len += n;
        self.modified = true;
        true
    }

    fn raw_remove(&mut self, pos: usize, n: usize) {
        if pos > self.len || n > self.len - pos { return; }
        self.buf.copy_within(pos + n..self.len, pos);
        self.len -= n;
        self.modified = true;
    }

    /// Insert `bytes` at the cursor as one undo step. False, with nothing
    /// changed, when they do not all fit.
    fn insert(&mut self, bytes: &[u8]) -> bool {
        if bytes.is_empty() { return true; }
        let pos = self.cursor;
        if !self.raw_insert(pos, bytes) { return false; }
        self.undo.push(Edit::Insert { pos: pos as u16, len: bytes.len() as u16 });
        self.cursor = pos + bytes.len();
        true
    }

    /// Remove the byte before the cursor.
    fn backspace(&mut self) {
        if self.cursor == 0 { return; }
        let pos = self.cursor - 1;
        self.undo.push(Edit::Delete { pos: pos as u16, ch: self.buf[pos], back: true });
        self.raw_remove(pos, 1);
        self.cursor = pos;
    }

    /// Remove the byte under the cursor.
    fn delete(&mut self) {
        if self.cursor >= self.len { return; }
        let pos = self.cursor;
        self.undo.push(Edit::Delete { pos: pos as u16, ch: self.buf[pos], back: false });
        self.raw_remove(pos, 1);
    }

    /// Take back the most recent edit. False when there is none.
    fn undo(&mut self) -> bool {
        let Some(e) = self.undo.pop() else { return false };
        match e {
            Edit::Insert { pos, len } => {
                let pos = pos as usize;
                self.raw_remove(pos, len as usize);
                self.cursor = pos.min(self.len);
            }
            Edit::Delete { pos, ch, back } => {
                let pos = pos as usize;
                if self.raw_insert(pos, &[ch]) {
                    self.cursor = if back { pos + 1 } else { pos };
                }
            }
        }
        true
    }

    // ── Movement ──

    /// Move the cursor up to `n` lines up or down, keeping its column where
    /// the target line is long enough. Returns the number of lines moved.
    /// On the first (last) line already, it goes to the start (end) of the
    /// buffer instead.
    fn move_lines(&mut self, n: usize, up: bool) -> usize {
        let mut start = self.line_start(self.cursor);
        let col = self.cursor - start;
        let mut moved = 0usize;
        while moved < n {
            if up {
                if start == 0 { break; }
                start = self.line_start(start - 1);
            } else {
                let end = self.line_end(start);
                if end >= self.len { break; }
                start = end + 1;
            }
            moved += 1;
        }
        if moved == 0 {
            self.cursor = if up { 0 } else { self.len };
        } else {
            self.cursor = start + col.min(self.line_end(start) - start);
        }
        moved
    }

    /// Scroll so the cursor is inside a view of `rows` x `cols` cells.
    fn scroll_to_cursor(&mut self, rows: usize, cols: usize) {
        let (line, col) = self.cursor_line_col();
        if line < self.top {
            self.top = line;
        } else if line >= self.top + rows {
            self.top = line + 1 - rows;
        }
        if col < self.left {
            self.left = col;
        } else if col >= self.left + cols {
            self.left = col + 1 - cols;
        }
    }
}

// ── Files ───────────────────────────────────────────────────────────────────

/// Written by the file browser: the name of the file to open.
const LAUNCH_FILE: [u8; 16] = *b"LAUNCH.TMP\0\0\0\0\0\0";

/// Same comparison the filesystem uses: bytes up to the first NUL.
fn same_name(a: &[u8; 16], b: &[u8; 16]) -> bool {
    for i in 0..16 {
        if a[i] != b[i] { return false; }
        if a[i] == 0 { return true; }
    }
    true
}

/// Size of file `name` according to the directory, if it exists.
fn dir_size(name: &[u8; 16]) -> Option<usize> {
    for i in 0..fcount() {
        let mut entry = [0u8; 16];
        let size = finfo(i, &mut entry);
        entry[15] = 0;
        if size >= 0 && same_name(&entry, name) { return Some(size as usize); }
    }
    None
}

fn is_elf_name(name: &[u8; 16]) -> bool {
    let len = text_len(name);
    len >= 4 && name[len - 4] == b'.' && name[len - 3..len].eq_ignore_ascii_case(b"ELF")
}

/// The file browser starts the editor on a file by writing the file's name
/// (the 16-byte, NUL-padded directory name) to LAUNCH.TMP and then
/// exec'ing the editor. The name is taken and LAUNCH.TMP deleted, so the
/// next start shows the picker again.
///
/// This hand-over is a single global file, not an argument: an editor
/// started from the desktop at the wrong moment picks up a name meant for
/// another one, two editors can both read it before either deletes it, and
/// a LAUNCH.TMP left behind (the browser died before exec, or the delete
/// below failed) makes the next editor open that file instead of the
/// picker. The content is not trusted beyond being a plausible name.
fn take_launch_name() -> Option<[u8; 16]> {
    let mut name = [0u8; 16];
    if fread(&LAUNCH_FILE, &mut name) <= 0 {
        return None;                            // no hand-over pending
    }
    if !fdelete(&LAUNCH_FILE) {
        println!("rs-edit: could not delete LAUNCH.TMP");
    }
    name[15] = 0;
    let len = text_len(&name);
    if len == 0 || name[..len].iter().any(|&b| !(32..=126).contains(&b)) {
        println!("rs-edit: LAUNCH.TMP did not hold a file name");
        return None;
    }
    for b in name[len..].iter_mut() { *b = 0; }
    Some(name)
}

/// A name for a new file that does not exist yet, so that saving it cannot
/// replace someone's file: NOTES.TXT, then NOTES1.TXT .. NOTES99.TXT.
fn new_file_name() -> Option<[u8; 16]> {
    for i in 0..100u32 {
        let mut name = [0u8; 16];
        let mut p = push_str(&mut name, 0, b"NOTES");
        if i > 0 { p = push_u32(&mut name, p, i); }
        push_str(&mut name, p, b".TXT");
        if dir_size(&name).is_none() { return Some(name); }
    }
    None
}

/// Load `name` into the editor. On failure says why in `msg` and returns
/// false; the editor is unchanged.
fn open_file(ed: &mut Editor, name: &[u8; 16], msg: &mut Msg) -> bool {
    match ed.load(name) {
        Ok(()) => true,
        Err(LoadError::Read) => {
            msg.set(RED).s(name).s(b": could not be read");
            false
        }
        Err(LoadError::TooLarge) => {
            msg.set(RED).s(name).s(b": ");
            match dir_size(name) {
                Some(size) => { msg.n(size).s(b" bytes"); }
                None       => { msg.s(b"too large"); }
            }
            msg.s(b", the editor holds at most ").n(BUF_CAP).s(b" - not opened");
            false
        }
    }
}

// ── File picker ─────────────────────────────────────────────────────────────

/// As many entries as a StinkFS directory can hold.
const PICK_MAX: usize = 80;

struct Picker {
    names:  [[u8; 16]; PICK_MAX],
    sizes:  [u32; PICK_MAX],
    count:  usize,
    more:   bool,    // the disk has more files than `PICK_MAX`
    sel:    usize,
    scroll: usize,
}

enum Pick {
    Stay,
    Quit,
    /// Start editing: the buffer is loaded (or empty, for a new file).
    Edit([u8; 16]),
}

impl Picker {
    const fn new() -> Picker {
        Picker {
            names: [[0u8; 16]; PICK_MAX], sizes: [0; PICK_MAX],
            count: 0, more: false, sel: 0, scroll: 0,
        }
    }

    /// List every file that is not a program.
    fn scan(&mut self) {
        self.count = 0;
        self.more = false;
        self.sel = 0;
        self.scroll = 0;
        for i in 0..fcount() {
            let mut name = [0u8; 16];
            let size = finfo(i, &mut name);
            name[15] = 0;
            if size < 0 || name[0] == 0 || is_elf_name(&name) { continue; }
            if self.count == PICK_MAX {
                self.more = true;
                break;
            }
            self.names[self.count] = name;
            self.sizes[self.count] = size as u32;
            self.count += 1;
        }
    }

    fn rows(win: &Window) -> usize {
        let r = (win.h() - STATUS_H - LIST_TOP - GAP) / ROW_H;
        if r < 1 { 1 } else { r as usize }
    }

    fn keep_visible(&mut self, rows: usize) {
        if self.sel < self.scroll {
            self.scroll = self.sel;
        } else if self.sel >= self.scroll + rows {
            self.scroll = self.sel + 1 - rows;
        }
    }

    fn key(&mut self, k: u8, ed: &mut Editor, msg: &mut Msg) -> Pick {
        msg.clear();
        match k {
            KEY_ESC | CTRL_Q => return Pick::Quit,
            KEY_UP => {
                if self.sel > 0 { self.sel -= 1; }
            }
            KEY_DOWN => {
                if self.sel + 1 < self.count { self.sel += 1; }
            }
            b'n' | b'N' => match new_file_name() {
                Some(name) => {
                    ed.reset();
                    msg.set(FG).s(b"New file: nothing is written until Ctrl+S");
                    return Pick::Edit(name);
                }
                None => {
                    msg.set(RED).s(b"No free name for a new file (NOTES.TXT .. NOTES99.TXT exist)");
                }
            },
            KEY_ENTER | b'\r' => {
                if self.count > 0 {
                    let name = self.names[self.sel];
                    if open_file(ed, &name, msg) { return Pick::Edit(name); }
                }
            }
            _ => {}
        }
        Pick::Stay
    }
}

// ── Drawing ─────────────────────────────────────────────────────────────────

/// The status bar's background, and the y of its text line.
fn status_base(win: &Window) -> i32 {
    status_bar(win, b"");
    win.h() - STATUS_H + 4
}

/// From `x` to the right margin of the status bar: the pending message, or
/// the key hints when there is none.
fn status_tail(win: &Window, x: i32, y: i32, msg: &Msg, hints: &[u8]) {
    let room = win.w() - PAD - x;
    if msg.is_set() {
        win.text_fit(x, y, &msg.text, room, msg.rgb);
    } else {
        win.text_fit(x, y, hints, room, FG_DIM);
    }
}

fn draw_picker(win: &Window, p: &Picker, msg: &Msg) {
    win.clear(BG);
    let w = win.w() - 2 * PAD;
    let caption: &[u8] = if p.more { b"Open a file (only the first 80 are listed)" } else { b"Open a file" };
    let top = section(win, PAD, PAD, w, caption);

    if p.count == 0 {
        win.text_fit(PAD, top, b"No files to open. Press N for a new one.", w, FG_DIM);
    }
    for v in 0..Picker::rows(win) {
        let i = p.scroll + v;
        if i >= p.count { break; }
        let r = Rect::new(PAD, top + v as i32 * ROW_H, w, ROW_H);
        list_row(win, r, &p.names[i], i == p.sel);
        if r.w >= SIZE_MIN_W {
            let mut size = [0u8; 24];
            let n = push_u32(&mut size, 0, p.sizes[i]);
            push_str(&mut size, n, b" bytes");
            win.text_right(r.x + r.w - GAP, r.y + (ROW_H - FONT_H) / 2, &size, FG_DIM);
        }
    }

    let y = status_base(win);
    status_tail(win, PAD, y, msg, b"Up/Down select   Enter open   N new file   Esc quit");
}

/// Text rows and columns that fit in the window (at least one of each, so
/// the scrolling arithmetic never sees zero).
fn text_rows(win: &Window) -> usize {
    let r = (win.h() - STATUS_H) / LINE_H;
    if r < 1 { 1 } else { r as usize }
}

fn text_cols(win: &Window) -> usize {
    clamp((win.w() - TEXT_X) / FONT_W, 1, ROW_BUF as i32) as usize
}

fn draw_editor(win: &Window, ed: &Editor, name: &[u8; 16], msg: &Msg) {
    let rows = text_rows(win);
    let cols = text_cols(win);
    let (cur_line, cur_col) = ed.cursor_line_col();

    win.clear(BG);
    win.fill(0, 0, GUTTER_W, win.h(), SURFACE);
    win.vline(GUTTER_W, 0, win.h(), BORDER);

    let mut start = ed.line_offset(ed.top);
    for v in 0..rows {
        let Some(s) = start else { break };       // past the last line
        let e = ed.line_end(s);
        let line = ed.top + v;
        let y = v as i32 * LINE_H;
        let current = line == cur_line;
        if current {
            win.fill(GUTTER_W + 1, y, win.w() - GUTTER_W - 1, LINE_H, SURFACE);
        }

        let mut num = [0u8; 12];
        fmt_u32(line as u32 + 1, &mut num);
        win.text_right(GUTTER_W - 4, y + 1, &num, if current { FG } else { FG_DIM });

        // The slice of this line inside the horizontal view.
        let from = (s + ed.left).min(e);
        let n = (e - from).min(cols);
        let mut row = [0u8; ROW_BUF];
        for i in 0..n {
            let b = ed.buf[from + i];
            row[i] = if (32..=126).contains(&b) { b } else { b'.' };
        }
        win.text(TEXT_X, y + 1, &row[..n], FG);

        if current && cur_col >= ed.left {
            let cx = TEXT_X + (cur_col - ed.left) as i32 * FONT_W;
            win.fill(cx, y + LINE_H - 2, FONT_W, 2, ACCENT);
        }

        start = if e < ed.len { Some(e + 1) } else { None };
    }

    // Status bar: file name, unsaved flag, line:column, then message or hints.
    let y = status_base(win);
    let right = win.w() - PAD;
    let mut x = PAD;
    x += win.text_fit(x, y, name, right - x, FG);
    if ed.modified {
        x += FONT_W;
        x += win.text_fit(x, y, b"[modified]", right - x, YELLOW);
    }
    let mut pos = [0u8; 24];
    let mut n = push_u32(&mut pos, 0, cur_line as u32 + 1);
    n = push_str(&mut pos, n, b":");
    push_u32(&mut pos, n, cur_col as u32 + 1);
    x += 2 * FONT_W;
    x += win.text_fit(x, y, &pos, right - x, FG_DIM);
    status_tail(win, x + 2 * FONT_W, y, msg,
                b"Ctrl+S save  Ctrl+Z undo  Ctrl+C copy all  Ctrl+V paste  Esc quit");
}

// ── Editing keys ────────────────────────────────────────────────────────────

fn save(ed: &mut Editor, name: &[u8; 16], msg: &mut Msg) {
    if fwrite(name, &ed.buf[..ed.len]) {
        ed.modified = false;
        msg.set(ACCENT).s(b"Saved ").n(ed.len).s(b" bytes to ").s(name);
    } else {
        msg.set(RED).s(b"SAVE FAILED: ").s(name).s(b" was not written");
    }
}

fn copy_all(ed: &Editor, msg: &mut Msg) {
    if ed.len == 0 {
        msg.set(FG).s(b"Nothing to copy");
        return;
    }
    let n = clip_write(&ed.buf[..ed.len]);
    if n == ed.len {
        msg.set(FG).s(b"Copied ").n(n).s(b" bytes");
    } else {
        msg.set(RED).s(b"Clipboard took only ").n(n).s(b" of ").n(ed.len).s(b" bytes");
    }
}

fn paste(ed: &mut Editor, msg: &mut Msg) {
    let mut clip = [0u8; 4096];
    let n = clip_read(&mut clip).min(clip.len());
    if n == 0 {
        msg.set(FG).s(b"Clipboard is empty");
    } else if ed.insert(&clip[..n]) {
        msg.set(FG).s(b"Pasted ").n(n).s(b" bytes");
    } else {
        msg.set(RED).s(b"Not pasted: ").n(n).s(b" bytes, only ").n(BUF_CAP - ed.len).s(b" free");
    }
}

fn type_bytes(ed: &mut Editor, bytes: &[u8], msg: &mut Msg) {
    if !ed.insert(bytes) {
        msg.set(RED).s(b"Buffer full (").n(BUF_CAP).s(b" bytes)");
    }
}

/// One key in the editor. `rows` is the page size for PgUp / PgDn.
/// Returns false when the key asks to quit.
fn edit_key(ed: &mut Editor, name: &[u8; 16], msg: &mut Msg, k: u8, rows: usize) -> bool {
    msg.clear();
    match k {
        KEY_ESC | CTRL_Q => return false,
        CTRL_S => save(ed, name, msg),
        CTRL_C => copy_all(ed, msg),
        CTRL_V => paste(ed, msg),
        CTRL_Z => {
            if !ed.undo() { msg.set(FG).s(b"Nothing to undo"); }
        }
        KEY_UP   => { ed.move_lines(1, true); }
        KEY_DOWN => { ed.move_lines(1, false); }
        KEY_LEFT => {
            if ed.cursor > 0 { ed.cursor -= 1; }
        }
        KEY_RIGHT => {
            if ed.cursor < ed.len { ed.cursor += 1; }
        }
        KEY_HOME => ed.cursor = ed.line_start(ed.cursor),
        KEY_END  => ed.cursor = ed.line_end(ed.cursor),
        // A page moves the view by as many lines as the cursor moved.
        KEY_PGUP => {
            let moved = ed.move_lines(rows, true);
            ed.top = ed.top.saturating_sub(moved);
        }
        KEY_PGDN => {
            let moved = ed.move_lines(rows, false);
            ed.top += moved;
        }
        KEY_BACKSPACE | KEY_DEL => ed.backspace(),
        KEY_ENTER | b'\r' => type_bytes(ed, b"\n", msg),
        32..=126 => type_bytes(ed, &[k], msg),
        _ => {}
    }
    true
}

// ── Main ────────────────────────────────────────────────────────────────────

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    println!("rs-edit: start");
    let Some(mut win) = Window::open(b"Text Editor\0", WIN_W, WIN_H, WIN_RESIZABLE) else {
        println!("rs-edit: could not open a window");
        quit(1);
    };

    let mut ed = Editor::new();
    let mut picker = Picker::new();
    let mut msg = Msg::new();
    let mut name = [0u8; 16];
    let mut editing = false;

    // A file handed over by the file browser skips the picker; if it cannot
    // be opened the picker comes up with the reason in its status bar.
    if let Some(launched) = take_launch_name() {
        if open_file(&mut ed, &launched, &mut msg) {
            name = launched;
            editing = true;
        }
    }
    if !editing {
        picker.scan();
    }

    let mut redraw = true;
    'run: loop {
        if redraw {
            if editing {
                ed.scroll_to_cursor(text_rows(&win), text_cols(&win));
                draw_editor(&win, &ed, &name, &msg);
            } else {
                picker.keep_visible(Picker::rows(&win));
                draw_picker(&win, &picker, &msg);
            }
            win.flush();
            redraw = false;
        }

        // Handle everything that is queued before drawing again.
        let mut next = win.wait(1000);
        while let Some(ev) = next {
            match ev {
                Event::Close => break 'run,
                Event::Resize { w, h } => {
                    if !win.resize(w, h) {
                        println!("rs-edit: resize refused, keeping the old size");
                    }
                    redraw = true;
                }
                Event::Key(k) => {
                    if editing {
                        if !edit_key(&mut ed, &name, &mut msg, k, text_rows(&win)) { break 'run; }
                    } else {
                        match picker.key(k, &mut ed, &mut msg) {
                            Pick::Quit => break 'run,
                            Pick::Edit(file) => {
                                name = file;
                                editing = true;
                            }
                            Pick::Stay => {}
                        }
                    }
                    redraw = true;
                }
                Event::Scan { code: SC_DELETE, pressed: true, extended: true } if editing => {
                    msg.clear();
                    ed.delete();
                    redraw = true;
                }
                _ => {}
            }
            next = win.poll();
        }
    }

    win.close();
    println!("rs-edit: exit");
}
