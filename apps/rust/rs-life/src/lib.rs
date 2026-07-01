//! Conway's Game of Life on the StinkOS framebuffer.
//!
//! 128x96 cell grid (8x8 px per cell on a 1024x768 fb). Starts with
//! the canonical Gosper glider gun shifted to the top-left so the
//! viewer immediately sees gliders streaming across the canvas.
//!
//! Controls (poll non-blocking):
//!   q     -> quit, clean exit code 0
//!   r     -> reload the seed pattern
//!   space -> pause / resume
//!
//! Output keys for tools/smoke-rs-life.py:
//!   "life: start"
//!   "life: gen=NNN alive=NNN"     (periodic, every ~50 generations)
//!   "life: bye"

#![no_std]
#![no_main]

extern crate alloc;

use alloc::vec::Vec;
use core::alloc::{GlobalAlloc, Layout};
use libstink::{println, exit};

extern "C" {
    fn malloc(n: usize) -> *mut u8;
    fn free(p: *mut u8);
    fn sys_draw(x: i32, y: i32, rgb: u32);
    fn sys_getkey() -> i32;
    fn sys_ticks() -> u32;
    fn draw_window_frame(x: i32, y: i32, w: i32, h: i32, title: *const u8) -> i32;
    fn sys_win_create(w: u32, h: u32) -> i32;
    fn sys_win_show(x: i32, y: i32, title: *const u8) -> i32;
    fn sys_win_flush();
    fn sys_win_destroy();
}

struct LibStinkAllocator;
unsafe impl GlobalAlloc for LibStinkAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if layout.align() > 8 { return core::ptr::null_mut(); }
        unsafe { malloc(layout.size()) }
    }
    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        unsafe { free(ptr) }
    }
}
#[global_allocator]
static ALLOC: LibStinkAllocator = LibStinkAllocator;

// ---- Grid ----

const COLS: usize = 128;
const ROWS: usize = 91;        // 91*8=728 ≤ 768-34 (fits below titlebar)
const CELL: i32 = 8;           // pixels per cell side
const OY:   i32 = 34;          // titlebar height offset
const BG:   u32 = 0x001022;   // dark blue-grey
const FG:   u32 = 0x00FF80;   // bright green
// Tick budget: ~10 generations/sec at 100 Hz PIT.
const TICKS_PER_GEN: u32 = 10;

struct Grid {
    cells: Vec<u8>,           // 0 = dead, 1 = alive; row-major
    next:  Vec<u8>,
}

impl Grid {
    fn new() -> Self {
        Self {
            cells: alloc::vec![0u8; COLS * ROWS],
            next:  alloc::vec![0u8; COLS * ROWS],
        }
    }

    fn at(&self, c: i32, r: i32) -> u8 {
        // Toroidal wrap: edges connect to the opposite side. Keeps
        // gliders alive when they leave the viewport.
        let cc = c.rem_euclid(COLS as i32) as usize;
        let rr = r.rem_euclid(ROWS as i32) as usize;
        self.cells[rr * COLS + cc]
    }

    fn set(&mut self, c: usize, r: usize, v: u8) {
        self.cells[r * COLS + c] = v;
    }

    fn step(&mut self) -> usize {
        let mut alive_count = 0usize;
        for r in 0..ROWS {
            for c in 0..COLS {
                let mut n: u8 = 0;
                for dr in -1i32..=1 {
                    for dc in -1i32..=1 {
                        if dr == 0 && dc == 0 { continue; }
                        n += self.at(c as i32 + dc, r as i32 + dr);
                    }
                }
                let cur = self.cells[r * COLS + c];
                // Standard B3/S23 Conway rules:
                let nxt = match (cur, n) {
                    (1, 2) | (1, 3) => 1,
                    (0, 3)          => 1,
                    _               => 0,
                };
                self.next[r * COLS + c] = nxt;
                if nxt == 1 { alive_count += 1; }
            }
        }
        // Swap buffers; old `cells` becomes the new scratch space.
        core::mem::swap(&mut self.cells, &mut self.next);
        alive_count
    }

    fn clear(&mut self) {
        for v in self.cells.iter_mut() { *v = 0; }
    }

    /// Gosper glider gun at the given top-left cell. The 9x36 pattern
    /// emits a glider every 30 generations toward the bottom-right.
    /// From John Conway / Bill Gosper 1970; canonical reference shape.
    fn seed_gun(&mut self, ox: usize, oy: usize) {
        const PAT: &[(usize, usize)] = &[
            (1, 5), (1, 6), (2, 5), (2, 6),
            (11, 5), (11, 6), (11, 7),
            (12, 4), (12, 8),
            (13, 3), (13, 9),
            (14, 3), (14, 9),
            (15, 6),
            (16, 4), (16, 8),
            (17, 5), (17, 6), (17, 7),
            (18, 6),
            (21, 3), (21, 4), (21, 5),
            (22, 3), (22, 4), (22, 5),
            (23, 2), (23, 6),
            (25, 1), (25, 2), (25, 6), (25, 7),
            (35, 3), (35, 4),
            (36, 3), (36, 4),
        ];
        for &(dx, dy) in PAT {
            let c = ox + dx;
            let r = oy + dy;
            if c < COLS && r < ROWS {
                self.set(c, r, 1);
            }
        }
    }
}

// ---- Render ----

fn paint_cell(c: usize, r: usize, on: bool) {
    let x0 = c as i32 * CELL;
    let y0 = r as i32 * CELL + OY;
    let color = if on { FG } else { BG };
    // Solid CELL x CELL block. Each sys_draw is one pixel; a 1024x768
    // fb has 786k pixels, so a full refresh of 128*96*64 = 786k calls
    // would tank perf. We rely on the diff path below to only repaint
    // changed cells, but the initial full-paint pays the full cost.
    for dy in 0..CELL {
        for dx in 0..CELL {
            unsafe { sys_draw(x0 + dx, y0 + dy, color); }
        }
    }
}

fn paint_full(g: &Grid) {
    for r in 0..ROWS {
        for c in 0..COLS {
            paint_cell(c, r, g.cells[r * COLS + c] != 0);
        }
    }
}

fn paint_diff(g: &Grid, prev: &[u8]) {
    for r in 0..ROWS {
        for c in 0..COLS {
            let idx = r * COLS + c;
            if g.cells[idx] != prev[idx] {
                paint_cell(c, r, g.cells[idx] != 0);
            }
        }
    }
}

// ---- App ----

fn wait_ticks(start: u32, n: u32) -> u32 {
    loop {
        let now = unsafe { sys_ticks() };
        if now.wrapping_sub(start) >= n {
            return now;
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    println!("life: start");
    unsafe { sys_win_create(1024, 768); sys_win_show(0, 0, b"Game of Life\0".as_ptr()); }

    unsafe { draw_window_frame(0, 0, 1024, 768,
        b"Conway's Game of Life  --  r: reset  space: pause  q: quit\0".as_ptr()); }
    let mut g = Grid::new();
    g.seed_gun(5, 5);
    paint_full(&g);

    let mut prev = g.cells.clone();
    let mut gen: u32 = 0;
    let mut paused = false;
    let mut last_tick = unsafe { sys_ticks() };

    loop {
        // Non-blocking key poll between gens. Use raw ASCII compare to
        // dodge dragging in a full keycode table.
        let k = unsafe { sys_getkey() };
        if k != 0 {
            let c = (k & 0xFF) as u8;
            match c {
                b'q' | b'Q' => { unsafe { sys_win_destroy(); } println!("life: bye"); exit(0); }
                b'r' | b'R' => {
                    g.clear();
                    g.seed_gun(5, 5);
                    paint_full(&g);
                    prev.copy_from_slice(&g.cells);
                    gen = 0;
                }
                b' ' => paused = !paused,
                _ => {}
            }
        }

        last_tick = wait_ticks(last_tick, TICKS_PER_GEN);
        if paused { continue; }

        let alive = g.step();
        paint_diff(&g, &prev);
        prev.copy_from_slice(&g.cells);
        gen += 1;
        unsafe { sys_win_flush(); }

        if gen % 50 == 0 {
            println!("life: gen={} alive={}", gen, alive);
        }
    }
}
