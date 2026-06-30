//! rs-vec: Lissajous vector art screensaver for StinkOS.
//!
//! Draws animated parametric curves using sys_drawline (Bresenham in userspace).
//! Traces X=A*sin(at + d), Y=B*sin(bt) — classic oscilloscope pattern.
//! Cycles through several frequency ratios; fades by redrawing in BG colour.
//!
//! Press any key to exit.

#![no_std]
#![no_main]

use libstink::println;
use libui::*;

const SCREEN_W: i32 = 1024;
const SCREEN_H: i32 = 768;
const WIN_W: i32 = 800;
const WIN_H: i32 = 640;
const WIN_X: i32 = (SCREEN_W - WIN_W) / 2;
const WIN_Y: i32 = (SCREEN_H - WIN_H) / 2;
const CX: i32 = WIN_X + WIN_W / 2;
const CY: i32 = WIN_Y + 34 + (WIN_H - 34) / 2;
const RX: i32 = WIN_W / 2 - 30;
const RY: i32 = (WIN_H - 34) / 2 - 20;

// ── Software trig (sin via Taylor; sufficient for visuals) ───────────────────

fn fmod(x: f64, m: f64) -> f64 {
    x - (x / m) as i64 as f64 * m
}

fn sinf(mut x: f64) -> f64 {
    // Reduce to [-π, π]
    const PI: f64 = 3.14159265358979323846;
    const TAU: f64 = 2.0 * PI;
    x = fmod(x, TAU);
    if x > PI  { x -= TAU; }
    if x < -PI { x += TAU; }
    // Taylor series order 9 (accurate to ~1e-9 for |x| ≤ π)
    let x2 = x * x;
    x * (1.0 - x2 / 6.0 * (1.0 - x2 / 20.0 * (1.0 - x2 / 42.0 * (1.0 - x2 / 72.0))))
}

fn cosf(x: f64) -> f64 {
    sinf(x + 3.14159265358979323846 / 2.0)
}

// ── Lissajous ratios ──────────────────────────────────────────────────────────

struct Ratio { a: f64, b: f64 }

const RATIOS: [Ratio; 6] = [
    Ratio { a: 1.0, b: 2.0 },
    Ratio { a: 1.0, b: 3.0 },
    Ratio { a: 2.0, b: 3.0 },
    Ratio { a: 3.0, b: 4.0 },
    Ratio { a: 3.0, b: 5.0 },
    Ratio { a: 4.0, b: 5.0 },
];

// ── Drawing ───────────────────────────────────────────────────────────────────

fn lissajous_point(t: f64, phase: f64, ratio: &Ratio) -> (i32, i32) {
    let x = CX + (cosf(ratio.a * t + phase) * RX as f64) as i32;
    let y = CY + (sinf(ratio.b * t) * RY as f64) as i32;
    (x, y)
}

fn draw_curve(ratio: &Ratio, phase: f64, color: u32, steps: usize) {
    const TAU: f64 = 6.28318530717958647;
    let dt = TAU / steps as f64;
    let (mut px, mut py) = lissajous_point(0.0, phase, ratio);
    for i in 1..=steps {
        let t = i as f64 * dt;
        let (nx, ny) = lissajous_point(t, phase, ratio);
        draw_line(px, py, nx, ny, color);
        px = nx; py = ny;
    }
}

// ── Main ──────────────────────────────────────────────────────────────────────

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    println!("rs-vec: start");

    fill(0, 0, SCREEN_W, SCREEN_H, BG);
    window_frame(WIN_X, WIN_Y, WIN_W, WIN_H, b"Vector Art  --  any key to exit\0");
    fill(WIN_X + 1, WIN_Y + 34, WIN_W - 2, WIN_H - 35, 0x020608);

    let content_bg: u32 = 0x020608;

    let mut ratio_idx = 0usize;
    let mut phase = 0.0f64;
    let mut frame = 0u32;
    const STEPS: usize = 300;
    const PHASE_STEP: f64 = 0.02;
    const FRAMES_PER_RATIO: u32 = 200;

    // Color cycle: cycle through accent-ish hues
    let colors: [u32; 6] = [
        0x57f287, // green
        0x5865f2, // blue
        0xfee75c, // yellow
        0xf47067, // red
        0xeb459e, // pink
        0x00b0f4, // cyan
    ];

    let mut prev_k = 0i32;

    loop {
        let k = poll_key();
        if k != 0 && k != prev_k && k != prev_k { break; }
        prev_k = k;

        // Erase previous curve in content-BG colour
        draw_curve(&RATIOS[ratio_idx], phase - PHASE_STEP, content_bg, STEPS);

        // Advance phase
        phase += PHASE_STEP;
        frame += 1;

        // Draw new curve
        let color = colors[ratio_idx % colors.len()];
        draw_curve(&RATIOS[ratio_idx], phase, color, STEPS);

        // Advance to next ratio periodically
        if frame >= FRAMES_PER_RATIO {
            // Erase fully
            fill(WIN_X + 1, WIN_Y + 34, WIN_W - 2, WIN_H - 35, content_bg);
            ratio_idx = (ratio_idx + 1) % RATIOS.len();
            phase  = 0.0;
            frame  = 0;
        }

        sleep_ms(30);
    }

    println!("rs-vec: exit");
}
