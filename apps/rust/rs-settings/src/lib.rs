//! rs-settings: system settings panel for StinkOS.
//!
//! Keyboard layout (US/BR) via SYS_SET_KEYMAP (syscall 76).
//! Sound test via SYS_SOUND (syscall 7).
//! Uses UiRow for button layout.

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

const CX: i32 = 20;
const BW: i32 = 120;
const BH: i32 = 36;
const BTN_GAP: i32 = 10;

static KB_BTNS: &[UiBtn] = &[
    UiBtn { id: 0, label: b"US (QWERTY)\0", w: BW, h: BH },
    UiBtn { id: 1, label: b"BR (ABNT2)\0",  w: BW, h: BH },
];
static SND_BTNS: &[UiBtn] = &[
    UiBtn { id: 2, label: b"Beep test\0", w: BW, h: BH },
    UiBtn { id: 3, label: b"Mute   \0",  w: BW, h: BH },
];
static SND_BTNS_MUTED: &[UiBtn] = &[
    UiBtn { id: 2, label: b"Beep test\0", w: BW, h: BH },
    UiBtn { id: 3, label: b"Muted  \0",  w: BW, h: BH },
];

fn section_header(label: &[u8], y: i32) {
    text16(CX, y, label, ACCENT);
    fill(CX, y + 11, WIN_W - 40, 1, BORDER);
}

fn render(mx: i32, my: i32, hov: u8, layout: i32, muted: bool) {
    fill(0, 0, WIN_W, WIN_H, BG);
    window_frame(0, 0, WIN_W, WIN_H, b"Settings\0");

    let content_y = 44;
    let kb_y      = content_y + 18;
    section_header(b"Keyboard Layout\0", content_y);

    let kb_row = UiRow { items: KB_BTNS, gap: BTN_GAP };
    kb_row.render(CX, kb_y, hov, layout as u8);

    let label_x = CX + kb_row.total_width() + 12;
    let layout_name: &[u8] = if layout == 0 { b"US (QWERTY)\0" } else { b"BR (ABNT2)\0" };
    text16(label_x,      kb_y + (BH - 8) / 2, b"active: \0",  FG_DIM);
    text16(label_x + 64, kb_y + (BH - 8) / 2, layout_name,    FG);

    let snd_section_y = kb_y + BH + 28;
    let snd_y         = snd_section_y + 18;
    section_header(b"Sound\0", snd_section_y);

    let snd_items = if muted { SND_BTNS_MUTED } else { SND_BTNS };
    let snd_row   = UiRow { items: snd_items, gap: BTN_GAP };
    snd_row.render(CX, snd_y, hov, if muted { 3 } else { 0xFF });

    let hint_y = WIN_H - 20;
    fill(8, hint_y - 4, WIN_W - 16, 1, BORDER);
    text16(CX, hint_y + 4, b"Q  close\0", FG_DIM);

    draw_cursor(mx, my);
}

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    println!("rs-settings: start");
    win_init_at(b"Settings ", WIN_W, WIN_H, WIN_X, WIN_Y);

    let content_y = 44;
    let kb_y      = content_y + 18;
    let snd_section_y = kb_y + BH + 28;
    let snd_y     = snd_section_y + 18;

    let mut mx      = WIN_W / 2;
    let mut my      = WIN_H / 2;
    let mut left_held = false;
    let mut hov: u8 = 0xFF;
    let mut layout  = 0i32;
    let mut muted   = false;
    let mut prev_k  = 0i32;
    let mut dirty   = true;

    loop {
        let (dx, dy, buttons) = poll_mouse();
        mx = clamp(mx + dx, 0, WIN_W - 1);
        my = clamp(my + dy, 0, WIN_H - 1);
        let left_now = buttons & 0x01 != 0;

        let kb_row   = UiRow { items: KB_BTNS, gap: BTN_GAP };
        let snd_items = if muted { SND_BTNS_MUTED } else { SND_BTNS };
        let snd_row  = UiRow { items: snd_items, gap: BTN_GAP };

        let new_hov = kb_row.hit_test(CX, kb_y, mx, my)
            .or_else(|| snd_row.hit_test(CX, snd_y, mx, my))
            .unwrap_or(0xFF);
        if new_hov != hov { hov = new_hov; dirty = true; }

        if !left_now && left_held {
            let clicked = kb_row.hit_test(CX, kb_y, mx, my)
                .or_else(|| snd_row.hit_test(CX, snd_y, mx, my));
            match clicked {
                Some(0) => { set_keymap(0); layout = 0; dirty = true; }
                Some(1) => { set_keymap(1); layout = 1; dirty = true; }
                Some(2) => { if !muted { sound(1000); sleep_ms(120); sound(0); } }
                Some(3) => { muted = !muted; if muted { sound(0); } dirty = true; }
                _ => {}
            }
        }
        left_held = left_now;

        let k = poll_key();
        if k != 0 && k != prev_k && (k == b'q' as i32 || k == 27) { break; }
        prev_k = k;

        if dirty {
            render(mx, my, hov, layout, muted);
            dirty = false;
        }
        win_flush();
        sleep_ms(16);
    }

    win_done();
    println!("rs-settings: exit");
}
