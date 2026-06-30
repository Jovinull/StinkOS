//! rs-desktop: graphical app launcher for StinkOS.
//!
//! Displays a grid of application tiles on the framebuffer with mouse cursor
//! support. Click a tile to launch the app via fork+exec; the desktop resumes
//! once the child exits. Press Q to quit back to the shell.
//!
//! ## Architecture
//!
//! - No window compositor required: the desktop owns the full framebuffer.
//! - App launch: fork() → child exec()s the app; parent waitpid()s.
//! - Redraw strategy: full-screen repaint on any mouse or hover change.
//!   Each repaint is fast because it uses sys_fillrect (kernel fills memory
//!   directly) and avoids per-pixel syscalls for the background.
//!
//! ## References
//!
//! - Z-order / focus management pattern: Orbital src/window_order.rs
//!   (VecDeque<WindowId> + zbuffer; simplified here to a single foreground).
//! - Tile state machine: Makepad widgets/src/button.rs (hover/down/normal).
//! - Window decorations: ToaruOS lib/decor-fancy.c (33px titlebar, close btn).

#![no_std]
#![no_main]

use libstink::println;
use libui::*;

// ── App catalogue ─────────────────────────────────────────────────────────────

struct AppEntry {
    label: &'static [u8],
    exec:  &'static [u8],
}

static APPS: &[AppEntry] = &[
    AppEntry { label: b"Doom\0",      exec: b"doom1\0"     },
    AppEntry { label: b"Asteroids\0", exec: b"asteroids\0" },
    AppEntry { label: b"Snake\0",     exec: b"snake\0"     },
    AppEntry { label: b"Pong\0",      exec: b"pong\0"      },
    AppEntry { label: b"Animation\0", exec: b"anim\0"      },
    AppEntry { label: b"Life (RS)\0", exec: b"rs-life\0"   },
    AppEntry { label: b"JSON (RS)\0", exec: b"rs-json\0"   },
    AppEntry { label: b"Stdio (RS)\0",exec: b"rs-stdio\0"  },
    AppEntry { label: b"FBDemo\0",    exec: b"fbdemo\0"    },
    AppEntry { label: b"Shell\0",     exec: b"shell\0"     },
];

// ── Layout constants ──────────────────────────────────────────────────────────

const SCREEN_W:   i32 = 1024;
const SCREEN_H:   i32 = 768;

const TILE_W:     i32 = 160;
const TILE_H:     i32 = 90;
const TILE_COLS:  i32 = 5;
const TILE_GAP_H: i32 = 20;
const TILE_GAP_V: i32 = 20;

const GRID_W:     i32 = TILE_COLS * TILE_W + (TILE_COLS - 1) * TILE_GAP_H;
const GRID_X:     i32 = (SCREEN_W - GRID_W) / 2;
const GRID_Y:     i32 = TASKBAR_H + 28;

// ── Tile helpers ──────────────────────────────────────────────────────────────

fn tile_at(idx: usize) -> Tile {
    let col = (idx as i32) % TILE_COLS;
    let row = (idx as i32) / TILE_COLS;
    Tile {
        label: APPS[idx].label,
        exec:  APPS[idx].exec,
        x: GRID_X + col * (TILE_W + TILE_GAP_H),
        y: GRID_Y + row * (TILE_H + TILE_GAP_V),
        w: TILE_W,
        h: TILE_H,
    }
}

fn hovered_idx(mx: i32, my: i32) -> Option<usize> {
    for i in 0..APPS.len() {
        if tile_at(i).contains(mx, my) { return Some(i); }
    }
    None
}

// ── Rendering ────────────────────────────────────────────────────────────────

fn redraw(mx: i32, my: i32) {
    /* Background fill */
    fill(0, 0, SCREEN_W, SCREEN_H, BG);

    /* Dot grid backdrop (every 32px, above the taskbar) */
    {
        let mut gx: i32 = 32;
        while gx < SCREEN_W {
            let mut gy: i32 = TASKBAR_H + 8;
            while gy < SCREEN_H {
                pixel(gx, gy, SURFACE);
                gy += 32;
            }
            gx += 32;
        }
    }

    /* Taskbar */
    draw_taskbar(SCREEN_W);

    /* Subtitle */
    text(16, TASKBAR_H + 10, b"Choose an app\0", FG_DIM);

    /* Section label */
    text(GRID_X, GRID_Y - 14, b"Applications\0", FG_DIM);

    /* App tiles */
    let hov = hovered_idx(mx, my);
    for i in 0..APPS.len() {
        tile_at(i).draw(hov == Some(i));
    }

    /* Cursor on top */
    draw_cursor(mx, my);
}

// ── App launch ────────────────────────────────────────────────────────────────

fn launch(exec_name: &'static [u8]) {
    let pid = fork();
    if pid == 0 {
        /* child: replace with the target app */
        exec(exec_name);
        quit(1); /* exec failed */
    } else if pid > 0 {
        /* parent: block until child exits, then resume desktop */
        wait_for(pid);
    }
    /* pid < 0 (fork failed): silently ignore */
}

// ── Main ──────────────────────────────────────────────────────────────────────

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    println!("rs-desktop: start");

    let mut mx: i32 = SCREEN_W / 2;
    let mut my: i32 = SCREEN_H / 2;
    let mut prev_mx   = mx - 1; /* ensure first draw */
    let mut prev_my   = my - 1;
    let mut prev_hov: Option<usize> = None;
    let mut left_held = false;

    loop {
        /* Mouse */
        let (dx, dy, buttons) = poll_mouse();
        mx = clamp(mx + dx, 0, SCREEN_W - 1);
        my = clamp(my + dy, 0, SCREEN_H - 1);

        /* Keyboard */
        let k = poll_key();
        if k == b'q' as i32 { break; }

        /* Click: rising edge of left button */
        let left_now = buttons & 0x01 != 0;
        if left_now && !left_held {
            if let Some(idx) = hovered_idx(mx, my) {
                launch(APPS[idx].exec);
                /* After launch, force full redraw */
                prev_mx = mx - 1;
            }
        }
        left_held = left_now;

        /* Redraw only when something visible changed */
        let hov = hovered_idx(mx, my);
        if mx != prev_mx || my != prev_my || hov != prev_hov {
            redraw(mx, my);
            prev_mx  = mx;
            prev_my  = my;
            prev_hov = hov;
        }

        sleep_ms(16); /* cap at ~60 fps */
    }

    println!("rs-desktop: exit");
}
