//! rs-todo: persistent TODO list for StinkOS.
//!
//! The list lives in the file TODO.DAT and is written back after every
//! change (add, toggle, delete), so what is on screen is what is on disk --
//! unless a write fails, which the window says in red until one succeeds.
//!
//! File format: one item per line, "[ ] text\n" (open) or "[X] text\n"
//! (done). The text is 0..=TEXT_MAX printable ASCII characters. At most
//! MAX_ITEMS items; an empty list is an empty file.
//!
//! Controls:
//!   A              add an item (type it, Enter to add, Esc to cancel)
//!   Space / Enter  toggle done on the selected item
//!   D              delete the selected item
//!   Up / Down      select
//!   S              write TODO.DAT now (retry after a failed save)
//!   Q / Esc        quit

#![no_std]
#![no_main]

use libstink::println;
use libui::*;

// ── Limits and format ───────────────────────────────────────────────────────

const MAX_ITEMS: usize = 32;
/// Characters in one item's text.
const TEXT_MAX: usize = 40;
/// "[X] " + text + '\n'
const LINE_MAX: usize = 4 + TEXT_MAX + 1;
/// Largest file this program writes.
const FILE_MAX: usize = MAX_ITEMS * LINE_MAX;
/// Read buffer: larger than `FILE_MAX`, so a file with lines this program
/// would not write (longer, malformed, too many) is still seen whole and
/// can be counted instead of cut off unnoticed.
const READ_CAP: usize = 2048;
const _: () = assert!(READ_CAP > FILE_MAX);

const FILE_NAME: [u8; 16] = *b"TODO.DAT\0\0\0\0\0\0\0\0";

// ── Layout ──────────────────────────────────────────────────────────────────

/// Rows of the list visible at once; it scrolls with the selection.
const VISIBLE: usize = 12;
/// The entry row sets the width: "New: " + text + cursor, a gap, "40/40".
const ENTRY_CHARS: i32 = 5 + TEXT_MAX as i32 + 1 + 1 + 5;
const ENTRY_INSET: i32 = 6;

const WIN_W: i32 = 2 * PAD + 2 * ENTRY_INSET + ENTRY_CHARS * FONT_W;
const LIST_TOP: i32 = PAD + FONT_H + 3 + GAP;               // under the section caption
const NOTE_Y:   i32 = LIST_TOP + VISIBLE as i32 * ROW_H + GAP;
const NOTE_H:   i32 = ROW_H + 4;
const WIN_H:    i32 = NOTE_Y + NOTE_H + GAP + STATUS_H;

// ── Data model ──────────────────────────────────────────────────────────────

#[derive(Clone, Copy)]
struct Item {
    text: [u8; TEXT_MAX],
    len:  usize,          // <= TEXT_MAX
    done: bool,
}

struct List {
    items: [Item; MAX_ITEMS],
    count: usize,         // <= MAX_ITEMS
}

impl List {
    const fn new() -> List {
        List { items: [Item { text: [0; TEXT_MAX], len: 0, done: false }; MAX_ITEMS], count: 0 }
    }

    /// Append an item. False when the list is full or the text too long.
    fn push(&mut self, text: &[u8], done: bool) -> bool {
        if self.count >= MAX_ITEMS || text.len() > TEXT_MAX { return false; }
        let it = &mut self.items[self.count];
        it.text[..text.len()].copy_from_slice(text);
        it.len = text.len();
        it.done = done;
        self.count += 1;
        true
    }

    fn remove(&mut self, i: usize) {
        if i >= self.count { return; }
        self.items.copy_within(i + 1..self.count, i);
        self.count -= 1;
    }
}

// ── Persistence ─────────────────────────────────────────────────────────────

/// Write the whole list to TODO.DAT. True when the file was written.
fn save(list: &List) -> bool {
    let mut buf = [0u8; FILE_MAX];
    let mut p = 0usize;
    for it in &list.items[..list.count] {
        buf[p..p + 4].copy_from_slice(if it.done { b"[X] " } else { b"[ ] " });
        p += 4;
        buf[p..p + it.len].copy_from_slice(&it.text[..it.len]);
        p += it.len;
        buf[p] = b'\n';
        p += 1;
    }
    fwrite(&FILE_NAME, &buf[..p])
}

/// What `load` found.
enum Loaded {
    /// The file was read. `lost` counts what could not be kept as it was:
    /// lines that are not items, lines cut to `TEXT_MAX`, items past
    /// `MAX_ITEMS`, and a file too big for the read buffer.
    Read { lost: u32 },
    /// There is no TODO.DAT yet.
    NoFile,
    /// TODO.DAT exists but could not be read.
    Failed,
}

/// One line without its '\n': `(done, text, cut)`, where `cut` says the
/// text was longer than `TEXT_MAX` and lost its tail. `None` when the line
/// is not "[ ] text" / "[X] text" in printable ASCII.
fn parse_line(line: &[u8]) -> Option<(bool, &[u8], bool)> {
    if line.len() < 4 || line[0] != b'[' || line[2] != b']' || line[3] != b' ' {
        return None;
    }
    let done = match line[1] {
        b' ' => false,
        b'X' | b'x' => true,
        _ => return None,
    };
    let text = &line[4..];
    if text.iter().any(|&b| !(32..=126).contains(&b)) {
        return None;
    }
    let cut = text.len() > TEXT_MAX;
    Some((done, &text[..text.len().min(TEXT_MAX)], cut))
}

/// Whether the directory has a TODO.DAT (a failed read cannot tell a
/// missing file from an unreadable one).
fn file_exists() -> bool {
    let want = &FILE_NAME[..text_len(&FILE_NAME)];
    (0..fcount()).any(|i| {
        let mut name = [0u8; 16];
        finfo(i, &mut name) >= 0 && &name[..text_len(&name)] == want
    })
}

/// Fill `list` from TODO.DAT. Whatever is valid is loaded; the rest is
/// counted, never guessed at.
fn load(list: &mut List) -> Loaded {
    list.count = 0;
    let mut buf = [0u8; READ_CAP];
    let n = fread(&FILE_NAME, &mut buf);
    if n < 0 {
        return if file_exists() { Loaded::Failed } else { Loaded::NoFile };
    }
    let total = (n as usize).min(READ_CAP);

    let (mut bad, mut cut, mut extra) = (0u32, 0u32, 0u32);
    let mut i = 0usize;
    while i < total {
        let mut end = i;
        while end < total && buf[end] != b'\n' { end += 1; }
        let line = &buf[i..end];
        i = end + 1;
        if line.is_empty() { continue; }            // blank lines carry nothing
        match parse_line(line) {
            None => bad += 1,
            Some((done, text, was_cut)) => {
                if !list.push(text, done) {
                    extra += 1;
                } else if was_cut {
                    cut += 1;
                }
            }
        }
    }
    // A full read buffer means the file goes on past what was read.
    let oversized = total == READ_CAP;

    if bad + cut + extra > 0 || oversized {
        println!("rs-todo: TODO.DAT: {} bad lines, {} cut to {} chars, {} past the {}-item limit{}",
                 bad, cut, TEXT_MAX, extra, MAX_ITEMS,
                 if oversized { ", file larger than the read buffer" } else { "" });
    }
    Loaded::Read { lost: bad + cut + extra + oversized as u32 }
}

// ── UI state ────────────────────────────────────────────────────────────────

/// A line of text for the notice row.
struct Note {
    text: [u8; 64],
    rgb:  u32,
}

impl Note {
    const fn new() -> Note { Note { text: [0; 64], rgb: FG } }
    fn clear(&mut self) { self.text[0] = 0; }
    fn is_set(&self) -> bool { self.text[0] != 0 }
}

struct Ui {
    sel:       usize,
    scroll:    usize,
    adding:    bool,              // typing a new item
    input:     [u8; TEXT_MAX],
    input_len: usize,
    unsaved:   bool,              // the last write of TODO.DAT failed
    note:      Note,              // about the last key; gone with the next one
    warn:      Note,              // about the file as loaded; gone once it is rewritten
}

impl Ui {
    const fn new() -> Ui {
        Ui {
            sel: 0, scroll: 0, adding: false, input: [0; TEXT_MAX], input_len: 0,
            unsaved: false, note: Note::new(), warn: Note::new(),
        }
    }

    /// Keep the selection inside the list and the visible rows.
    fn settle(&mut self, count: usize) {
        if self.sel >= count { self.sel = count.saturating_sub(1); }
        if self.sel < self.scroll {
            self.scroll = self.sel;
        } else if self.sel >= self.scroll + VISIBLE {
            self.scroll = self.sel + 1 - VISIBLE;
        }
        self.scroll = self.scroll.min(count.saturating_sub(VISIBLE));
    }

    /// Write the list after a change. A failure stays flagged (and shown)
    /// until a later write succeeds.
    fn persist(&mut self, list: &List) -> bool {
        let ok = save(list);
        self.unsaved = !ok;
        if ok {
            self.warn.clear();
        } else {
            println!("rs-todo: writing TODO.DAT failed");
        }
        ok
    }
}

// ── Drawing ─────────────────────────────────────────────────────────────────

fn draw_notice(win: &Window, list: &List, ui: &Ui) {
    let w = win.w() - 2 * PAD;
    let r = Rect::new(PAD, NOTE_Y, w, NOTE_H);
    let ty = r.y + (r.h - FONT_H) / 2;
    let tx = r.x + ENTRY_INSET;
    let room = r.w - 2 * ENTRY_INSET;

    if ui.adding {
        // Entry field: prompt, text, cursor; used/limit at the right.
        win.fill_rect(r, SURFACE);
        win.rect(r, ACCENT);
        let mut x = tx + win.text(tx, ty, b"New: ", ACCENT);
        x += win.text(x, ty, &ui.input[..ui.input_len], FG);
        win.text(x, ty, b"_", ACCENT);

        let mut used = [0u8; 16];
        let mut n = push_u32(&mut used, 0, ui.input_len as u32);
        n = push_str(&mut used, n, b"/");
        push_u32(&mut used, n, TEXT_MAX as u32);
        let full = ui.input_len >= TEXT_MAX;
        win.text_right(r.x + r.w - ENTRY_INSET, ty, &used, if full { YELLOW } else { FG_DIM });
    } else if ui.unsaved {
        win.text_fit(tx, ty, b"NOT SAVED: writing TODO.DAT failed. S tries again.", room, RED);
    } else if ui.note.is_set() {
        win.text_fit(tx, ty, &ui.note.text, room, ui.note.rgb);
    } else if ui.warn.is_set() {
        win.text_fit(tx, ty, &ui.warn.text, room, ui.warn.rgb);
    } else if list.count > VISIBLE {
        let mut s = [0u8; 48];
        let mut n = push_str(&mut s, 0, b"Items ");
        n = push_u32(&mut s, n, ui.scroll as u32 + 1);
        n = push_str(&mut s, n, b"-");
        n = push_u32(&mut s, n, (ui.scroll + VISIBLE).min(list.count) as u32);
        n = push_str(&mut s, n, b" of ");
        push_u32(&mut s, n, list.count as u32);
        win.text_fit(tx, ty, &s, room, FG_DIM);
    }
}

fn draw(win: &Window, list: &List, ui: &Ui) {
    win.clear(BG);
    let w = win.w() - 2 * PAD;

    // Caption with the done / total count at its right end.
    section(win, PAD, PAD, w, b"Tasks");
    let done = list.items[..list.count].iter().filter(|it| it.done).count();
    let mut stats = [0u8; 24];
    let mut n = push_u32(&mut stats, 0, done as u32);
    n = push_str(&mut stats, n, b"/");
    n = push_u32(&mut stats, n, list.count as u32);
    push_str(&mut stats, n, b" done");
    win.text_right(PAD + w, PAD, &stats, FG_DIM);

    if list.count == 0 {
        win.text(PAD + ENTRY_INSET, LIST_TOP + 2, b"No items. Press A to add one.", FG_DIM);
    }
    for v in 0..VISIBLE {
        let i = ui.scroll + v;
        if i >= list.count { break; }
        let it = &list.items[i];
        let r = Rect::new(PAD, LIST_TOP + v as i32 * ROW_H, w, ROW_H);
        if i == ui.sel {
            win.fill_rect(r, SURFACE_ALT);
            win.fill(r.x, r.y, 3, r.h, ACCENT);
        }
        let ty = r.y + (r.h - FONT_H) / 2;
        let tx = r.x + 10;
        win.text(tx, ty, if it.done { b"[x]" } else { b"[ ]" }, if it.done { ACCENT } else { FG_DIM });
        win.text_fit(tx + 4 * FONT_W, ty, &it.text[..it.len], r.x + r.w - (tx + 4 * FONT_W),
                     if it.done { FG_DIM } else { FG });
    }

    draw_notice(win, list, ui);

    status_bar(win, if ui.adding {
        b"Enter add   Esc cancel   Backspace erase" as &[u8]
    } else {
        b"A add  Space toggle  D delete  S save  Q quit"
    });
}

// ── Keys ────────────────────────────────────────────────────────────────────

/// A key while typing a new item.
fn input_key(list: &mut List, ui: &mut Ui, k: u8) {
    match k {
        KEY_ENTER | b'\r' => {
            // An empty entry adds nothing, same as cancelling.
            if ui.input_len > 0 && list.push(&ui.input[..ui.input_len], false) {
                ui.sel = list.count - 1;
                ui.persist(list);
            }
            ui.adding = false;
        }
        KEY_ESC => ui.adding = false,
        KEY_BACKSPACE | 127 => {
            if ui.input_len > 0 { ui.input_len -= 1; }
        }
        32..=126 => {
            // At the limit further characters are dropped; the counter in
            // the entry row turns yellow there.
            if ui.input_len < TEXT_MAX {
                ui.input[ui.input_len] = k;
                ui.input_len += 1;
            }
        }
        _ => {}
    }
}

/// A key in the list. Returns false when it asks to quit.
fn list_key(list: &mut List, ui: &mut Ui, k: u8) -> bool {
    ui.note.clear();
    match k {
        b'q' | b'Q' | KEY_ESC => return false,
        b'a' | b'A' => {
            if list.count >= MAX_ITEMS {
                ui.note.rgb = YELLOW;
                let n = push_str(&mut ui.note.text, 0, b"The list is full (");
                let n = push_u32(&mut ui.note.text, n, MAX_ITEMS as u32);
                push_str(&mut ui.note.text, n, b" items). Delete one first.");
            } else {
                ui.adding = true;
                ui.input_len = 0;
            }
        }
        b'd' | b'D' => {
            if list.count > 0 {
                list.remove(ui.sel);
                ui.persist(list);
            }
        }
        b' ' | KEY_ENTER | b'\r' => {
            if ui.sel < list.count {
                list.items[ui.sel].done = !list.items[ui.sel].done;
                ui.persist(list);
            }
        }
        b's' | b'S' => {
            if ui.persist(list) {
                ui.note.rgb = ACCENT;
                let n = push_str(&mut ui.note.text, 0, b"Saved ");
                let n = push_u32(&mut ui.note.text, n, list.count as u32);
                push_str(&mut ui.note.text, n, b" items to TODO.DAT");
            }
        }
        KEY_UP => {
            if ui.sel > 0 { ui.sel -= 1; }
        }
        KEY_DOWN => {
            if ui.sel + 1 < list.count { ui.sel += 1; }
        }
        _ => {}
    }
    true
}

// ── Main ────────────────────────────────────────────────────────────────────

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    println!("rs-todo: start");
    let Some(mut win) = Window::open(b"TODO List\0", WIN_W, WIN_H, 0) else {
        println!("rs-todo: could not open a window");
        quit(1);
    };

    let mut list = List::new();
    let mut ui = Ui::new();
    match load(&mut list) {
        Loaded::NoFile | Loaded::Read { lost: 0 } => {}
        Loaded::Read { lost } => {
            // The valid items are shown; the first change rewrites the file
            // without the rest, so say it before that happens.
            ui.warn.rgb = YELLOW;
            let n = push_str(&mut ui.warn.text, 0, b"TODO.DAT: ");
            let n = push_u32(&mut ui.warn.text, n, lost);
            push_str(&mut ui.warn.text, n, b" bad entries; next save drops them");
        }
        Loaded::Failed => {
            println!("rs-todo: TODO.DAT exists but could not be read");
            ui.warn.rgb = RED;
            push_str(&mut ui.warn.text, 0, b"TODO.DAT could not be read; a save replaces it");
        }
    }

    let mut redraw = true;
    'run: loop {
        if redraw {
            ui.settle(list.count);
            draw(&win, &list, &ui);
            win.flush();
            redraw = false;
        }

        // Handle everything that is queued before drawing again.
        let mut next = win.wait(1000);
        while let Some(ev) = next {
            match ev {
                Event::Close => break 'run,
                Event::Key(k) => {
                    if ui.adding {
                        input_key(&mut list, &mut ui, k);
                    } else if !list_key(&mut list, &mut ui, k) {
                        break 'run;
                    }
                    // Later keys in this batch must see a valid selection.
                    ui.settle(list.count);
                    redraw = true;
                }
                _ => {}
            }
            next = win.poll();
        }
    }

    // Every change was written when it was made; only a failed write is
    // still pending here, so give it one more try.
    if ui.unsaved && !save(&list) {
        println!("rs-todo: TODO.DAT could not be written; the last changes are lost");
    }

    win.close();
    println!("rs-todo: exit");
}
