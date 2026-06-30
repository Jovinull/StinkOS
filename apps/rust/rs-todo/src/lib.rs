//! rs-todo: persistent TODO list for StinkOS.
//!
//! Items saved to/loaded from StinkFS file "TODO.DAT".
//! Format: lines of "[ ] text\n" or "[X] text\n".
//!
//! Controls:
//!   A         — add new item (inline text input)
//!   Space/Enter — toggle done on selected item
//!   D         — delete selected item
//!   Up/Down   — navigate
//!   S         — save to TODO.DAT
//!   Q/Esc     — quit (prompts save if dirty)

#![no_std]
#![no_main]

use libstink::println;
use libui::*;

// ── Layout ────────────────────────────────────────────────────────────────────

const SCREEN_W: i32 = 1024;
const SCREEN_H: i32 = 768;
const WIN_W: i32 = 640;
const WIN_H: i32 = 540;
const WIN_X: i32 = (SCREEN_W - WIN_W) / 2;
const WIN_Y: i32 = (SCREEN_H - WIN_H) / 2;
const CX: i32 = WIN_X + 16;
const ROW_H: i32 = 22;

const MAX_ITEMS: usize = 32;
const ITEM_CAP:  usize = 44; // max chars per item (40 printable + "[X] " + nul)
const FILE_NAME: &[u8] = b"TODO.DAT\0";

// ── Data model ────────────────────────────────────────────────────────────────

struct Item {
    text: [u8; ITEM_CAP],
    len:  usize,
    done: bool,
}

impl Item {
    const fn empty() -> Self {
        Item { text: [0u8; ITEM_CAP], len: 0, done: false }
    }
}

// ── Persistence ───────────────────────────────────────────────────────────────

fn save(items: &[Item; MAX_ITEMS], count: usize) {
    let mut buf = [0u8; MAX_ITEMS * (ITEM_CAP + 5)];
    let mut p = 0usize;
    for i in 0..count {
        buf[p] = b'['; p += 1;
        buf[p] = if items[i].done { b'X' } else { b' ' }; p += 1;
        buf[p] = b']'; p += 1;
        buf[p] = b' '; p += 1;
        let l = items[i].len;
        for j in 0..l { buf[p] = items[i].text[j]; p += 1; }
        buf[p] = b'\n'; p += 1;
    }
    fwrite(FILE_NAME, &buf[..p]);
}

fn load(items: &mut [Item; MAX_ITEMS]) -> usize {
    let mut buf = [0u8; MAX_ITEMS * (ITEM_CAP + 5)];
    let n = fread(FILE_NAME, &mut buf);
    if n <= 0 { return 0; }

    let mut count = 0usize;
    let mut i = 0usize;
    let total = n as usize;
    while i < total && count < MAX_ITEMS {
        // Expect "[X] text\n" or "[ ] text\n"
        if i + 4 >= total { break; }
        if buf[i] != b'[' { while i < total && buf[i] != b'\n' { i += 1; } i += 1; continue; }
        let done = buf[i + 1] == b'X';
        // Skip "] " (buf[i+2] == ']', buf[i+3] == ' ')
        i += 4;
        // Read text until \n
        let text_start = i;
        while i < total && buf[i] != b'\n' { i += 1; }
        let text_len = (i - text_start).min(ITEM_CAP - 1);
        items[count].done = done;
        items[count].len  = text_len;
        for j in 0..text_len { items[count].text[j] = buf[text_start + j]; }
        items[count].text[text_len] = 0;
        count += 1;
        if i < total { i += 1; } // skip \n
    }
    count
}

// ── Rendering ─────────────────────────────────────────────────────────────────

fn draw_list(items: &[Item; MAX_ITEMS], count: usize, sel: usize,
             scroll: usize, visible: usize, dirty_flag: bool) {
    fill(0, 0, SCREEN_W, SCREEN_H, BG);
    let title = if dirty_flag { b"TODO  (unsaved)\0" as &[u8] } else { b"TODO\0" };
    window_frame(WIN_X, WIN_Y, WIN_W, WIN_H, title);
    fill(WIN_X + 1, WIN_Y + 34, WIN_W - 2, WIN_H - 35, SURFACE);

    let list_top = WIN_Y + 34 + 8;

    if count == 0 {
        text16(CX, list_top + 10, b"No items. Press A to add.\0", FG_DIM);
    } else {
        for vi in 0..visible {
            let idx = scroll + vi;
            if idx >= count { break; }

            let ry     = list_top + vi as i32 * ROW_H;
            let is_sel = idx == sel;
            let bg     = if is_sel { SURFACE_ALT } else { SURFACE };
            fill(WIN_X + 1, ry, WIN_W - 2, ROW_H - 1, bg);
            if is_sel { fill(WIN_X + 1, ry, 3, ROW_H - 1, ACCENT); }

            // Checkbox
            let cbx = CX + 2;
            let done = items[idx].done;
            let check_col = if done { 0x57c96d } else { FG_DIM };
            text16(cbx, ry + (ROW_H - 8) / 2, if done { b"[x]\0" } else { b"[ ]\0" }, check_col);

            // Text
            let text_col = if done { FG_DIM } else if is_sel { FG } else { FG };
            let mut tmp = [0u8; ITEM_CAP];
            let l = items[idx].len.min(ITEM_CAP - 1);
            tmp[..l].copy_from_slice(&items[idx].text[..l]);
            tmp[l] = 0;
            text16(cbx + 32, ry + (ROW_H - 8) / 2, &tmp, text_col);
        }
    }

    // Stats line
    let done_count = items[..count].iter().filter(|it| it.done).count();
    let stats_y = WIN_Y + WIN_H - 36;
    fill(WIN_X + 1, stats_y - 2, WIN_W - 2, 1, BORDER);
    let mut sbuf = [0u8; 32];
    let mut sp = 0usize;
    let mut tmp2 = [0u8; 12];
    let l = fmt_u32(done_count as u32, &mut tmp2);
    for i in 0..l { sbuf[sp] = tmp2[i]; sp += 1; }
    sbuf[sp] = b'/'; sp += 1;
    let l2 = fmt_u32(count as u32, &mut tmp2);
    for i in 0..l2 { sbuf[sp] = tmp2[i]; sp += 1; }
    let suffix = b" done\0";
    for b in suffix { sbuf[sp] = *b; sp += 1; if *b == 0 { break; } }
    text16(CX, stats_y + 2, &sbuf, FG_DIM);

    // Hint bar
    let hint_y = WIN_Y + WIN_H - 18;
    fill(WIN_X + 1, hint_y - 4, WIN_W - 2, 1, BORDER);
    text16(CX, hint_y, b"A add  Space toggle  D del  S save  Q quit\0", FG_DIM);
}

// ── Inline input ──────────────────────────────────────────────────────────────

fn read_line(items: &[Item; MAX_ITEMS], count: usize, sel: usize,
             scroll: usize, visible: usize, dirty: bool) -> ([u8; ITEM_CAP], usize) {
    let prompt_y = WIN_Y + WIN_H - 54;
    fill(WIN_X + 1, prompt_y - 4, WIN_W - 2, 1, BORDER);
    fill(WIN_X + 1, prompt_y, WIN_W - 2, 20, 0x0e1a0e);
    text16(CX, prompt_y + 6, b"New item: \0", ACCENT);

    let mut buf = [0u8; ITEM_CAP];
    let mut len = 0usize;
    let mut prev_k = 0i32;
    let input_x = CX + 80;

    loop {
        // Draw input area
        fill(input_x - 2, prompt_y + 2, WIN_W - 16 - 80, 16, 0x0e1a0e);
        let mut disp = [0u8; ITEM_CAP];
        disp[..len].copy_from_slice(&buf[..len]);
        disp[len] = b'_';
        disp[len + 1] = 0;
        text16(input_x, prompt_y + 6, &disp, FG);

        let k = poll_key();
        if k != 0 && k != prev_k {
            match k {
                k if k == b'\n' as i32 || k == b'\r' as i32 => break,
                k if k == 27 => { len = 0; break; }
                k if k == 8 || k == 127 => {
                    if len > 0 { len -= 1; }
                }
                k if k >= 32 && k < 127 && len < 40 => {
                    buf[len] = k as u8; len += 1;
                }
                _ => {}
            }
        }
        prev_k = k;

        // Redraw list behind input
        draw_list(items, count, sel, scroll, visible, dirty);
        fill(WIN_X + 1, prompt_y - 4, WIN_W - 2, 1, BORDER);
        fill(WIN_X + 1, prompt_y, WIN_W - 2, 20, 0x0e1a0e);
        text16(CX, prompt_y + 6, b"New item: \0", ACCENT);

        sleep_ms(16);
    }

    buf[len] = 0;
    (buf, len)
}

// ── Main ──────────────────────────────────────────────────────────────────────

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    println!("rs-todo: start");

    let mut items: [Item; MAX_ITEMS] = core::array::from_fn(|_| Item::empty());
    let mut count = load(&mut items);
    let mut sel    = 0usize;
    let mut scroll = 0usize;
    let mut dirty  = false;
    let mut prev_k = 0i32;

    let list_top = WIN_Y + 34 + 8;
    let list_bot = WIN_Y + WIN_H - 56;
    let visible  = ((list_bot - list_top) / ROW_H) as usize;

    let mut need_draw = true;

    loop {
        if need_draw {
            draw_list(&items, count, sel, scroll, visible, dirty);
            need_draw = false;
        }

        let k = poll_key();
        if k != 0 && k != prev_k {
            match k {
                k if k == b'q' as i32 || k == 27 => break,
                k if k == b's' as i32 || k == b'S' as i32 => {
                    save(&items, count);
                    dirty = false;
                    need_draw = true;
                }
                k if k == b'a' as i32 || k == b'A' as i32 => {
                    if count < MAX_ITEMS {
                        let (buf, len) = read_line(&items, count, sel, scroll, visible, dirty);
                        if len > 0 {
                            items[count].text[..len].copy_from_slice(&buf[..len]);
                            items[count].text[len] = 0;
                            items[count].len  = len;
                            items[count].done = false;
                            count += 1;
                            sel   = count - 1;
                            if sel >= scroll + visible { scroll = sel + 1 - visible; }
                            dirty = true;
                        }
                    }
                    need_draw = true;
                }
                k if k == b'd' as i32 || k == b'D' as i32 => {
                    if count > 0 {
                        for i in sel..count - 1 { items[i] = Item::empty(); items[i].done = items[i+1].done; items[i].len = items[i+1].len; items[i].text = items[i+1].text; }
                        items[count - 1] = Item::empty();
                        count -= 1;
                        if sel >= count && count > 0 { sel = count - 1; }
                        dirty = true;
                        need_draw = true;
                    }
                }
                k if k == b' ' as i32 || k == b'\n' as i32 || k == b'\r' as i32 => {
                    if count > 0 {
                        items[sel].done = !items[sel].done;
                        dirty = true;
                        need_draw = true;
                    }
                }
                k if k == KEY_UP => {
                    if sel > 0 {
                        sel -= 1;
                        if sel < scroll { scroll = sel; }
                        need_draw = true;
                    }
                }
                k if k == KEY_DOWN => {
                    if count > 0 && sel + 1 < count {
                        sel += 1;
                        if sel >= scroll + visible { scroll = sel + 1 - visible; }
                        need_draw = true;
                    }
                }
                _ => {}
            }
        }
        prev_k = k;
        sleep_ms(16);
    }

    println!("rs-todo: exit");
}
