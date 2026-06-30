//! rs-paint: pixel drawing app for StinkOS.
//!
//! Left button: draw with current colour and brush size.
//! Right button: erase (paint canvas background).
//! Bottom toolbar: click a colour swatch to pick it; 1/2/3 = brush 2/6/12px.
//! C: clear canvas. Q or Esc: quit.

#![no_std]
#![no_main]

use libstink::println;
use libui::*;

// ── Layout ────────────────────────────────────────────────────────────────────

const SCREEN_W: i32 = 1024;
const SCREEN_H: i32 = 768;

// Titlebar from draw_window_frame = 33 + 1 separator = 34
const TITLE_H:  i32 = 34;

// Bottom toolbar strip
const TOOL_H:   i32 = 40;
const CANVAS_Y: i32 = TITLE_H;
const CANVAS_H: i32 = SCREEN_H - TITLE_H - TOOL_H;
const TOOL_Y:   i32 = SCREEN_H - TOOL_H;

const CANVAS_BG: u32 = 0xfafafa;  // near-white canvas

// ── Colour palette (16 swatches) ─────────────────────────────────────────────

const PALETTE: [u32; 16] = [
    0x000000, // black
    0xffffff, // white
    0xe74c3c, // red
    0x2ecc71, // green
    0x3498db, // blue
    0xf1c40f, // yellow
    0x1abc9c, // cyan
    0x9b59b6, // purple
    0xe67e22, // orange
    0xe91e63, // pink
    0x795548, // brown
    0x607d8b, // slate
    0x8bc34a, // lime
    0xff5722, // deep orange
    0x00bcd4, // teal
    0x9e9e9e, // grey
];

const SWATCH_W:  i32 = 32;
const SWATCH_H:  i32 = 32;
const SWATCH_Y:  i32 = TOOL_Y + (TOOL_H - SWATCH_H) / 2;
const PALETTE_X: i32 = 8;

fn swatch_x(idx: usize) -> i32 {
    PALETTE_X + idx as i32 * (SWATCH_W + 2)
}

// ── Drawing helpers ───────────────────────────────────────────────────────────

fn draw_toolbar(sel_colour: u32, brush: i32, line_mode: bool) {
    fill(0, TOOL_Y, SCREEN_W, TOOL_H, 0x090e14);
    fill(0, TOOL_Y, SCREEN_W, 1, BORDER);

    for i in 0..PALETTE.len() {
        let sx = swatch_x(i);
        fill(sx, SWATCH_Y, SWATCH_W, SWATCH_H, PALETTE[i]);
        // Highlight selected colour with accent border
        if PALETTE[i] == sel_colour {
            fill(sx - 2, SWATCH_Y - 2, SWATCH_W + 4, 2, ACCENT);
            fill(sx - 2, SWATCH_Y + SWATCH_H, SWATCH_W + 4, 2, ACCENT);
            fill(sx - 2, SWATCH_Y - 2, 2, SWATCH_H + 4, ACCENT);
            fill(sx + SWATCH_W, SWATCH_Y - 2, 2, SWATCH_H + 4, ACCENT);
        }
    }

    // Tool status (right side)
    let bx = SCREEN_W - 200;
    let mode_label: &[u8] = if line_mode { b"Tool: Line  \0" } else { b"Tool: Brush \0" };
    text16(bx, TOOL_Y + 6, mode_label, if line_mode { ACCENT } else { FG });
    let mut bs = [0u8; 16];
    bs[0] = b'S'; bs[1] = b'z'; bs[2] = b':'; bs[3] = b' ';
    bs[4] = b'0' + (brush % 10) as u8;
    bs[5] = b' '; bs[6] = b'p'; bs[7] = b'x'; bs[8] = 0;
    text16(bx, TOOL_Y + 22, &bs, FG_DIM);
    text16(bx + 80, TOOL_Y + 22, b"1/2/3 L C Q\0", FG_DIM);
}

fn draw_brush(mx: i32, my: i32, colour: u32, brush: i32) {
    if my < CANVAS_Y || my >= CANVAS_Y + CANVAS_H { return; }
    let half = brush / 2;
    let bx = (mx - half).max(0);
    let by = (my - half).max(CANVAS_Y);
    let bw = (brush).min(SCREEN_W - bx);
    let bh = (brush).min(CANVAS_Y + CANVAS_H - by);
    if bw > 0 && bh > 0 {
        fill(bx, by, bw, bh, colour);
    }
}

fn pick_colour(mx: i32, my: i32) -> Option<u32> {
    if my < SWATCH_Y || my >= SWATCH_Y + SWATCH_H { return None; }
    for i in 0..PALETTE.len() {
        let sx = swatch_x(i);
        if mx >= sx && mx < sx + SWATCH_W {
            return Some(PALETTE[i]);
        }
    }
    None
}

// ── Main ──────────────────────────────────────────────────────────────────────

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    println!("rs-paint: start");

    // Initial screen setup
    fill(0, 0, SCREEN_W, SCREEN_H, BG);
    window_frame(0, 0, SCREEN_W, SCREEN_H, b"Paint\0");
    fill(0, CANVAS_Y, SCREEN_W, CANVAS_H, CANVAS_BG);
    draw_toolbar(PALETTE[0], 6, false);

    let mut mx: i32 = SCREEN_W / 2;
    let mut my: i32 = SCREEN_H / 2;
    let mut colour     = PALETTE[0];
    let mut erase_col  = CANVAS_BG;
    let mut brush: i32 = 6;
    let mut left_held  = false;
    let mut right_held = false;
    let mut line_mode  = false;
    let mut line_start = (0i32, 0i32);
    let mut prev_k     = 0i32;

    loop {
        let (dx, dy, buttons) = poll_mouse();
        mx = clamp(mx + dx, 0, SCREEN_W - 1);
        my = clamp(my + dy, 0, SCREEN_H - 1);

        let left_now  = buttons & 0x01 != 0;
        let right_now = buttons & 0x02 != 0;

        // Click on toolbar: pick colour
        if left_now && !left_held && my >= TOOL_Y {
            if let Some(c) = pick_colour(mx, my) {
                colour = c;
                draw_toolbar(colour, brush, line_mode);
            }
        }

        if !line_mode {
            // Brush draw / erase on canvas
            if left_now && my >= CANVAS_Y && my < CANVAS_Y + CANVAS_H {
                draw_brush(mx, my, colour, brush);
            }
            if right_now && my >= CANVAS_Y && my < CANVAS_Y + CANVAS_H {
                draw_brush(mx, my, erase_col, brush);
            }
        } else {
            // Line tool: record start on press, draw on release
            if left_now && !left_held && my >= CANVAS_Y && my < CANVAS_Y + CANVAS_H {
                line_start = (mx, my);
            }
            if !left_now && left_held {
                if my >= CANVAS_Y && my < CANVAS_Y + CANVAS_H {
                    draw_line(line_start.0, line_start.1, mx, my, colour);
                }
            }
        }

        left_held  = left_now;
        right_held = right_now;

        // Keyboard
        let k = poll_key();
        if k != 0 && k != prev_k {
            match k {
                k if k == b'q' as i32 || k == 27 => break,
                k if k == b'c' as i32 || k == b'C' as i32 => {
                    fill(0, CANVAS_Y, SCREEN_W, CANVAS_H, CANVAS_BG);
                }
                k if k == b'l' as i32 || k == b'L' as i32 => {
                    line_mode = !line_mode;
                    draw_toolbar(colour, brush, line_mode);
                }
                k if k == b'1' as i32 => { brush = 2;  draw_toolbar(colour, brush, line_mode); }
                k if k == b'2' as i32 => { brush = 6;  draw_toolbar(colour, brush, line_mode); }
                k if k == b'3' as i32 => { brush = 12; draw_toolbar(colour, brush, line_mode); }
                _ => {}
            }
        }
        prev_k = k;
        let _ = right_held;

        // Cursor (on top of everything)
        draw_cursor(mx, my);

        sleep_ms(16);
    }

    println!("rs-paint: exit");
}
