//! rs-desktop: the StinkOS desktop.
//!
//! A full-screen window without chrome that sits behind every other window
//! and shows the programs on the disk, grouped by kind. Clicking a tile (or
//! moving to it with the arrow keys and pressing Enter) starts the program
//! as its own process; the desktop keeps running and stays responsive, and
//! collects its children as they exit.
//!
//! Windowed programs appear on top, with a button in the kernel's taskbar.
//! The entries under "Full screen" are the older programs that draw straight
//! to the framebuffer: one of those takes over the whole screen until it
//! exits, then the desktop comes back.
//!
//! Ctrl+Q (or the "Text menu" tile) ends the graphical session and returns
//! to the kernel's text menu.

#![no_std]
#![no_main]

use libstink::println;
use libui::*;

struct App {
    label: &'static [u8],
    exec:  &'static [u8],
}

struct Group {
    title: &'static [u8],
    apps:  &'static [App],
}

/// `exec` value of the tile that ends the session instead of starting a program.
const EXIT: &[u8] = b"\0";

static GROUPS: &[Group] = &[
    Group { title: b"Applications\0", apps: &[
        App { label: b"Files\0",     exec: b"rs-files\0" },
        App { label: b"Editor\0",    exec: b"rs-edit\0" },
        App { label: b"Paint\0",     exec: b"rs-paint\0" },
        App { label: b"Calculator\0", exec: b"rs-calc\0" },
        App { label: b"Hex View\0",  exec: b"rs-hex\0" },
        App { label: b"TODO\0",      exec: b"rs-todo\0" },
        App { label: b"Calendar\0",  exec: b"rs-cal\0" },
        App { label: b"Clock\0",     exec: b"rs-clock\0" },
        App { label: b"Vector Art\0", exec: b"rs-vec\0" },
    ] },
    Group { title: b"System\0", apps: &[
        App { label: b"System Info\0", exec: b"rs-sysinfo\0" },
        App { label: b"Network\0",   exec: b"rs-net\0" },
        App { label: b"Tasks\0",     exec: b"rs-taskman\0" },
        App { label: b"Settings\0",  exec: b"rs-settings\0" },
        App { label: b"About\0",     exec: b"rs-about\0" },
        App { label: b"Text menu\0", exec: EXIT },
    ] },
    Group { title: b"Full screen\0", apps: &[
        App { label: b"Doom\0",      exec: b"doom1\0" },
        App { label: b"Shell\0",     exec: b"shell\0" },
        App { label: b"Asteroids\0", exec: b"asteroids\0" },
        App { label: b"Snake\0",     exec: b"snake\0" },
        App { label: b"Pong\0",      exec: b"pong\0" },
        App { label: b"Breakout\0",  exec: b"breakout\0" },
        App { label: b"Life\0",      exec: b"rs-life\0" },
        App { label: b"Animation\0", exec: b"anim\0" },
    ] },
];

// ── Layout ──────────────────────────────────────────────────────────────────

const DESK_W: i32 = SCREEN_W;
const DESK_H: i32 = SCREEN_H - TASKBAR_H;

const MARGIN:  i32 = 48;
const TILE_W:  i32 = 146;
const TILE_H:  i32 = 44;
const TILE_GAP: i32 = 10;
const COLS:    i32 = (DESK_W - 2 * MARGIN + TILE_GAP) / (TILE_W + TILE_GAP);
const HEAD_H:  i32 = 76;            // title block above the first group
const GROUP_GAP: i32 = 22;

/// Total number of tiles.
fn count() -> usize {
    GROUPS.iter().map(|g| g.apps.len()).sum()
}

/// The app behind flat index `i`.
fn app(i: usize) -> &'static App {
    let mut k = i;
    for g in GROUPS {
        if k < g.apps.len() { return &g.apps[k]; }
        k -= g.apps.len();
    }
    &GROUPS[0].apps[0]
}

/// Where tile `i` is drawn. Groups stack vertically; each wraps at COLS.
fn tile_rect(i: usize) -> Rect {
    let mut y = HEAD_H;
    let mut k = i as i32;
    for g in GROUPS {
        let n = g.apps.len() as i32;
        let rows = (n + COLS - 1) / COLS;
        y += FONT_H + 3 + GAP;                       // the group caption
        if k < n {
            return Rect::new(MARGIN + (k % COLS) * (TILE_W + TILE_GAP),
                             y + (k / COLS) * (TILE_H + TILE_GAP), TILE_W, TILE_H);
        }
        k -= n;
        y += rows * (TILE_H + TILE_GAP) - TILE_GAP + GROUP_GAP;
    }
    Rect::new(0, 0, 0, 0)
}

fn tile_at(x: i32, y: i32) -> Option<usize> {
    (0..count()).find(|&i| tile_rect(i).contains(x, y))
}

// ── Drawing ─────────────────────────────────────────────────────────────────

fn draw_tile(win: &Window, i: usize, hot: bool, pressed: bool) {
    let r = tile_rect(i);
    let a = app(i);
    let (bg, fg, edge) = if pressed { (ACCENT, BG, ACCENT) }
                         else if hot { (SURFACE_ALT, FG, ACCENT) }
                         else { (SURFACE, FG, BORDER) };
    win.fill_rect(r, bg);
    win.rect(r, edge);
    // A small mark on the left: dim for ordinary tiles, accent when hot.
    win.fill(r.x + 10, r.y + (r.h - 8) / 2, 3, 8, if pressed { BG } else if hot { ACCENT } else { BORDER });
    win.text_fit(r.x + 22, r.y + (r.h - FONT_H) / 2, a.label, r.w - 30, fg);
}

fn draw_all(win: &Window, hot: Option<usize>, pressed: Option<usize>, running: u32) {
    win.clear(BG);

    win.text(MARGIN, 22, b"StinkOS\0", ACCENT);
    win.text(MARGIN + 8 * FONT_W + 12, 22, b"desktop\0", FG_DIM);
    win.text(MARGIN, 44, b"Click a tile, or use the arrow keys and Enter.  Ctrl+Q: text menu.\0", FG_DIM);

    let mut status = [0u8; 40];
    let mut p = push_u32(&mut status, 0, running);
    p = push_str(&mut status, p, if running == 1 { b" program running\0" } else { b" programs running\0" });
    let _ = p;
    win.text_right(DESK_W - MARGIN, 22, &status, FG_DIM);

    let mut idx = 0usize;
    for g in GROUPS {
        let first = tile_rect(idx);
        section(win, MARGIN, first.y - (FONT_H + 3 + GAP), DESK_W - 2 * MARGIN, g.title);
        for _ in g.apps {
            draw_tile(win, idx, hot == Some(idx), pressed == Some(idx));
            idx += 1;
        }
    }
}

// ── Keyboard navigation ─────────────────────────────────────────────────────

/// Move from tile `i` by one step in a direction, staying on a tile.
fn step(i: usize, key: u8) -> usize {
    let n = count() as i32;
    let i = i as i32;
    let r = tile_rect(i as usize);
    let next = match key {
        KEY_LEFT  => i - 1,
        KEY_RIGHT => i + 1,
        // Up/down: the nearest tile in the adjacent row, by position.
        KEY_UP | KEY_DOWN => {
            let mut best = i;
            let mut best_d = i32::MAX;
            for k in 0..n {
                let t = tile_rect(k as usize);
                let above = key == KEY_UP && t.y < r.y;
                let below = key == KEY_DOWN && t.y > r.y;
                if !(above || below) { continue; }
                let dy = (t.y - r.y).abs();
                let dx = (t.x - r.x).abs();
                let d = dy * 4 + dx;                 // prefer the closest row, then column
                if d < best_d { best_d = d; best = k; }
            }
            best
        }
        _ => i,
    };
    clamp(next, 0, n - 1) as usize
}

// ── Main ────────────────────────────────────────────────────────────────────

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    println!("rs-desktop: start");
    let Some(mut win) = Window::open(b"Desktop\0", DESK_W, DESK_H, WIN_DESKTOP) else {
        println!("rs-desktop: no window, giving up");
        quit(1);
    };

    // Where every tile is, on the serial console: lets the QA scripts click
    // a program by name instead of by hard-coded coordinates.
    for i in 0..count() {
        let r = tile_rect(i);
        let e = app(i).exec;
        let name = core::str::from_utf8(&e[..text_len(e)]).unwrap_or("?");
        println!("desktop: tile {} {} {} {} {}", if name.is_empty() { "exit" } else { name }, r.x, r.y, r.w, r.h);
    }
    println!("desktop: ready");

    let mut hot: Option<usize> = None;       // tile under the pointer or keyboard cursor
    let mut pressed: Option<usize> = None;   // tile the left button went down on
    let mut running: u32 = 0;
    let mut dirty = true;

    loop {
        if dirty {
            draw_all(&win, hot, pressed, running);
            win.flush();
            dirty = false;
        }

        // Children that exited since the last pass.
        while reap().is_some() {
            running = running.saturating_sub(1);
            dirty = true;
        }

        let Some(ev) = win.wait(500) else { continue };
        match ev {
            Event::Mouse { x, y, .. } => {
                let over = tile_at(x, y);
                if ev.left_press().is_some() {
                    pressed = over;
                    dirty = true;
                } else if ev.left_release().is_some() {
                    // A click counts when press and release land on the same tile.
                    if pressed.is_some() && pressed == over {
                        if launch(over.unwrap()) { running += 1; } else if app(over.unwrap()).exec == EXIT { break; }
                    }
                    pressed = None;
                    dirty = true;
                }
                if over != hot {
                    hot = over;
                    dirty = true;
                }
            }
            Event::Key(k) if k == ctrl(b'q') => break,
            Event::Key(k @ (KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN)) => {
                hot = Some(match hot { Some(i) => step(i, k), None => 0 });
                dirty = true;
            }
            Event::Key(KEY_ENTER) => {
                if let Some(i) = hot {
                    if launch(i) { running += 1; dirty = true; } else if app(i).exec == EXIT { break; }
                }
            }
            _ => {}
        }
    }

    win.close();
    println!("rs-desktop: exit");
}

/// Start the program behind tile `i`. False for the exit tile or when the
/// process could not be created.
fn launch(i: usize) -> bool {
    let a = app(i);
    if a.exec == EXIT {
        return false;
    }
    let pid = spawn(a.exec);
    if pid < 0 {
        println!("rs-desktop: could not start a process");
        return false;
    }
    true
}
