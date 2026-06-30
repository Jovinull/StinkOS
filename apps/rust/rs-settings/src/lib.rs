//! rs-settings: system settings panel for StinkOS.
//!
//! Keyboard layout (US/BR) via SYS_SET_KEYMAP (syscall 76).
//! Sound test via SYS_SOUND (syscall 7).
//! Mouse-driven Button widget from libui.

#![no_std]
#![no_main]

use libstink::println;
use libui::*;

const SCREEN_W: i32 = 1024;
const SCREEN_H: i32 = 768;
const WIN_W: i32 = 480;
const WIN_H: i32 = 380;
const WIN_X: i32 = (SCREEN_W - WIN_W) / 2;
const WIN_Y: i32 = (SCREEN_H - WIN_H) / 2;

const CX: i32 = WIN_X + 20;

// Button dimensions
const BW: i32 = 120;
const BH: i32 = 36;
const BTN_GAP: i32 = 10;

// ── Section helpers ───────────────────────────────────────────────────────────

fn section_header(label: &[u8], y: i32) {
    text(CX, y, label, ACCENT);
    fill(CX, y + 11, WIN_W - 40, 1, BORDER);
}

// ── Rendering ─────────────────────────────────────────────────────────────────

fn render(mx: i32, my: i32, pressed: Option<usize>, layout: i32, muted: bool) {
    fill(0, 0, SCREEN_W, SCREEN_H, BG);
    window_frame(WIN_X, WIN_Y, WIN_W, WIN_H, b"Settings\0");

    let content_y = WIN_Y + 44;

    // ── Keyboard section ─────────────────────────────────────────────────────
    section_header(b"Keyboard Layout\0", content_y);

    let kb_y = content_y + 18;
    let btns: &[(usize, &[u8], i32)] = &[
        (0, b"US (QWERTY)\0", 0),
        (1, b"BR (ABNT2)\0",  1),
    ];
    for (idx, label, kl) in btns.iter() {
        let bx = CX + *idx as i32 * (BW + BTN_GAP);
        let is_active = layout == *kl;
        let hov = mx >= bx && mx < bx + BW && my >= kb_y && my < kb_y + BH;
        let state = if pressed == Some(*idx) {
            BtnState::Pressed
        } else if is_active {
            BtnState::Pressed // use Pressed style to show active
        } else if hov {
            BtnState::Hovered
        } else {
            BtnState::Normal
        };
        Button { label, x: bx, y: kb_y, w: BW, h: BH }.draw(state);
    }
    let layout_name: &[u8] = if layout == 0 { b"US (QWERTY)\0" } else { b"BR (ABNT2)\0" };
    text(CX + 2*(BW + BTN_GAP) + 8, kb_y + (BH - 8) / 2, b"active: \0", FG_DIM);
    text(CX + 2*(BW + BTN_GAP) + 72, kb_y + (BH - 8) / 2, layout_name, FG);

    // ── Sound section ────────────────────────────────────────────────────────
    let snd_section_y = kb_y + BH + 28;
    section_header(b"Sound\0", snd_section_y);

    let snd_y = snd_section_y + 18;
    let snd_btns: &[(usize, &[u8])] = &[
        (2, b"Beep test\0"),
        (3, if muted { b"Muted  \0" } else { b"Mute   \0" }),
    ];
    for (idx, label) in snd_btns.iter() {
        let bx = CX + (*idx - 2) as i32 * (BW + BTN_GAP);
        let hov = mx >= bx && mx < bx + BW && my >= snd_y && my < snd_y + BH;
        let is_active = *idx == 3 && muted;
        let state = if pressed == Some(*idx) || is_active {
            BtnState::Pressed
        } else if hov {
            BtnState::Hovered
        } else {
            BtnState::Normal
        };
        Button { label, x: bx, y: snd_y, w: BW, h: BH }.draw(state);
    }

    // ── Quit hint ────────────────────────────────────────────────────────────
    let hint_y = WIN_Y + WIN_H - 20;
    fill(WIN_X + 8, hint_y - 4, WIN_W - 16, 1, BORDER);
    text(CX, hint_y + 4, b"Q  close\0", FG_DIM);

    draw_cursor(mx, my);
}

// ── Main ──────────────────────────────────────────────────────────────────────

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    println!("rs-settings: start");

    let mut mx: i32 = SCREEN_W / 2;
    let mut my: i32 = SCREEN_H / 2;
    let mut left_held   = false;
    let mut pressed_idx: Option<usize> = None;
    let mut layout      = 0i32; // 0=US, 1=BR
    let mut muted       = false;
    let mut prev_k      = 0i32;
    let mut dirty       = true;
    let mut prev_mx     = mx - 1;
    let mut prev_my     = my - 1;

    loop {
        let (dx, dy, buttons) = poll_mouse();
        mx = clamp(mx + dx, 0, SCREEN_W - 1);
        my = clamp(my + dy, 0, SCREEN_H - 1);

        let left_now = buttons & 0x01 != 0;

        // Click: detect which button was hit
        if left_now && !left_held {
            // Find hit button index by checking bounding boxes
            let content_y = WIN_Y + 44;
            let kb_y      = content_y + 18;
            let snd_y     = kb_y + BH + 28 + 18;

            for idx in 0..4usize {
                let (bx, by) = match idx {
                    0 | 1 => (CX + idx as i32 * (BW + BTN_GAP), kb_y),
                    2 | 3 => (CX + (idx - 2) as i32 * (BW + BTN_GAP), snd_y),
                    _ => continue,
                };
                if mx >= bx && mx < bx + BW && my >= by && my < by + BH {
                    pressed_idx = Some(idx);
                    dirty = true;
                    break;
                }
            }
        }

        // Release: fire action
        if !left_now && left_held {
            if let Some(idx) = pressed_idx {
                match idx {
                    0 => { layout = set_keymap(0); dirty = true; }
                    1 => { layout = 1 - set_keymap(1); layout = 1; dirty = true; } // set BR, ignore prev
                    2 => {
                        if !muted { sound(1000); sleep_ms(120); sound(0); }
                    }
                    3 => {
                        muted = !muted;
                        if muted { sound(0); }
                        dirty = true;
                    }
                    _ => {}
                }
                pressed_idx = None;
            }
        }
        if mx != prev_mx || my != prev_my { dirty = true; }
        prev_mx = mx; prev_my = my;
        left_held = left_now;

        let k = poll_key();
        if k != 0 && k != prev_k {
            if k == b'q' as i32 || k == 27 { break; }
        }
        prev_k = k;

        if dirty {
            render(mx, my, pressed_idx, layout, muted);
            dirty = false;
        }

        sleep_ms(16);
    }

    println!("rs-settings: exit");
}
