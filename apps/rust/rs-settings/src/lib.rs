//! rs-settings: keyboard layout and a PC speaker test.
//!
//! Two buttons choose the keyboard layout (US or BR) for the whole system;
//! the panel shows the layout the kernel is using, not the one it was last
//! asked for from here. A third button plays a short tone on the PC
//! speaker. Esc, Q or the close box end it.

#![no_std]
#![no_main]

use libstink::println;
use libui::*;

/// Layout numbers of `set_keymap`.
const LAYOUT_US: i32 = 0;
const LAYOUT_BR: i32 = 1;

/// The PIT ticks at 100 Hz.
const TICKS_PER_SEC: u32 = 100;
const TONE_HZ: u32 = 1000;
/// How long the test tone sounds: 0.3 s.
const TONE_TICKS: u32 = 3 * TICKS_PER_SEC / 10;

/// With nothing to time, sleep this long between looks at the queue.
const IDLE_MS: u32 = 60_000;

// ── Layout ──────────────────────────────────────────────────────────────────

/// What `section` takes: caption, rule, spacing.
const SECTION_H: i32 = FONT_H + 3 + GAP;

const BTN_W: i32 = 136;
const WIN_W: i32 = 2 * PAD + 2 * BTN_W + GAP;

const KB_SECTION_Y:  i32 = PAD;
const KB_BTN_Y:      i32 = KB_SECTION_Y + SECTION_H;
const KB_FIELD_Y:    i32 = KB_BTN_Y + BTN_H + GAP;
const SND_SECTION_Y: i32 = KB_FIELD_Y + ROW_H + GAP;
const SND_BTN_Y:     i32 = SND_SECTION_Y + SECTION_H;
const SND_FIELD_Y:   i32 = SND_BTN_Y + BTN_H + GAP;
const WIN_H:         i32 = SND_FIELD_Y + ROW_H + GAP + STATUS_H;

// The buttons, by index.
const BTN_US:   usize = 0;
const BTN_BR:   usize = 1;
const BTN_TONE: usize = 2;

static BUTTONS: [Button; 3] = [
    Button::new(Rect::new(PAD, KB_BTN_Y, BTN_W, BTN_H), b"US (QWERTY)\0"),
    Button::new(Rect::new(PAD + BTN_W + GAP, KB_BTN_Y, BTN_W, BTN_H), b"BR (ABNT2)\0"),
    Button::new(Rect::new(PAD, SND_BTN_Y, BTN_W, BTN_H), b"Play tone\0"),
];

fn button_at(x: i32, y: i32) -> Option<usize> {
    BUTTONS.iter().position(|b| b.hit(x, y))
}

// ── Keyboard layout ─────────────────────────────────────────────────────────

/// The layout the kernel is using right now.
///
/// `set_keymap` is the only call there is, and it always sets: it returns
/// the layout that was active before. So select US, read the answer, and
/// put BR back if that is what it was. The layout is system-wide and may
/// have been changed by another program, so it is asked for rather than
/// assumed.
fn active_layout() -> i32 {
    let was = set_keymap(LAYOUT_US);
    if was == LAYOUT_BR {
        set_keymap(LAYOUT_BR);
    }
    was
}

fn layout_name(layout: i32) -> &'static [u8] {
    match layout {
        LAYOUT_US => b"US (QWERTY)\0",
        LAYOUT_BR => b"BR (ABNT2)\0",
        _ => b"unknown\0",
    }
}

// ── Drawing ─────────────────────────────────────────────────────────────────

struct Panel {
    layout:   i32,               // what the kernel last reported or was set to
    hot:      Option<usize>,     // button under the pointer
    armed:    Option<usize>,     // button the left button went down on
    /// Tick at which the test tone is to be switched off; None: not playing.
    tone_end: Option<u32>,
}

fn draw(win: &Window, p: &Panel) {
    win.clear(BG);
    let w = win.w() - 2 * PAD;
    let right = PAD + w;

    section(win, PAD, KB_SECTION_Y, w, b"Keyboard layout\0");
    section(win, PAD, SND_SECTION_Y, w, b"PC speaker\0");

    for (i, b) in BUTTONS.iter().enumerate() {
        let state = if p.armed == Some(i) && p.hot == Some(i) { BtnState::Pressed }
                    else if p.armed.is_none() && p.hot == Some(i) { BtnState::Hover }
                    else { BtnState::Normal };
        b.draw(win, state);
        // The active layout's button carries the selection marker.
        let active = (i == BTN_US && p.layout == LAYOUT_US) || (i == BTN_BR && p.layout == LAYOUT_BR);
        if active && state != BtnState::Pressed {
            win.fill(b.rect.x, b.rect.y, 3, b.rect.h, ACCENT);
        }
    }

    field(win, PAD, KB_FIELD_Y, right, b"Active layout\0", layout_name(p.layout), FG);

    let mut label = [0u8; 32];
    let mut n = push_str(&mut label, 0, b"Test tone, \0");
    n = push_u32(&mut label, n, TONE_HZ);
    push_str(&mut label, n, b" Hz\0");
    if p.tone_end.is_some() {
        field(win, PAD, SND_FIELD_Y, right, &label, b"playing\0", ACCENT);
    } else {
        field(win, PAD, SND_FIELD_Y, right, &label, b"off\0", FG_DIM);
    }

    status_bar(win, b"Esc: close\0");
}

// ── Main ────────────────────────────────────────────────────────────────────

/// True once tick `now` has reached tick `at` (the counter may wrap).
fn reached(now: u32, at: u32) -> bool {
    now.wrapping_sub(at) < 0x8000_0000
}

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    println!("rs-settings: start");
    let Some(mut win) = Window::open(b"Settings\0", WIN_W, WIN_H, 0) else {
        println!("rs-settings: could not open a window");
        quit(1);
    };

    let mut p = Panel { layout: active_layout(), hot: None, armed: None, tone_end: None };
    let mut dirty = true;

    loop {
        // The test tone ends on time whatever else is going on.
        if let Some(end) = p.tone_end {
            if reached(ticks(), end) {
                sound(0);
                p.tone_end = None;
                dirty = true;
            }
        }
        if dirty {
            draw(&win, &p);
            win.flush();
            dirty = false;
        }

        // Sleep until the tone is due to end, or until input arrives.
        let ms = match p.tone_end {
            Some(end) => {
                let left = end.wrapping_sub(ticks());
                if left == 0 || left > TONE_TICKS { 10 } else { left * 10 }
            }
            None => IDLE_MS,
        };
        let Some(ev) = win.wait(ms) else { continue };

        match ev {
            Event::Close | Event::Key(KEY_ESC | b'q' | b'Q') => break,
            Event::Mouse { x, y, .. } => {
                let over = button_at(x, y);
                if ev.left_press().is_some() {
                    p.armed = over;
                    dirty = true;
                } else if ev.left_release().is_some() {
                    // A click counts when press and release land on the same button.
                    if p.armed.is_some() && p.armed == over {
                        match over {
                            Some(BTN_US) => {
                                set_keymap(LAYOUT_US);
                                p.layout = LAYOUT_US;
                            }
                            Some(BTN_BR) => {
                                set_keymap(LAYOUT_BR);
                                p.layout = LAYOUT_BR;
                            }
                            Some(BTN_TONE) => {
                                sound(TONE_HZ);
                                p.tone_end = Some(ticks().wrapping_add(TONE_TICKS));
                            }
                            _ => {}
                        }
                    }
                    p.armed = None;
                    dirty = true;
                }
                if over != p.hot {
                    p.hot = over;
                    dirty = true;
                }
            }
            // Another program may have changed the layout while this
            // window was in the background: ask again when it comes back.
            Event::Focus(true) => {
                let now = active_layout();
                if now != p.layout {
                    p.layout = now;
                    dirty = true;
                }
            }
            _ => {}
        }
    }

    // A tone still sounding is ours: do not leave it on.
    if p.tone_end.is_some() {
        sound(0);
    }
    win.close();
    println!("rs-settings: exit");
}
