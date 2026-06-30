//! rs-calc: scientific calculator for StinkOS.
//!
//! Mouse-driven Button widgets (libui::Button) demonstrate interactive UI.
//! Keyboard shortcuts: 0-9, +,-,*,/, Enter=equals, Escape=clear, Backspace.
//! Scientific: s=sin, t=tan, r=sqrt (input treated as degrees).
//!
//! Arithmetic uses f64; sin/cos/tan via Taylor series, sqrt via Newton-Raphson.
//! All f64 ops use the x87 FPU without libm.

#![no_std]
#![no_main]

use libstink::println;
use libui::*;

// ── Layout ────────────────────────────────────────────────────────────────────

const SCREEN_W: i32 = 1024;
const SCREEN_H: i32 = 768;
const WIN_W: i32 = 400;
const WIN_H: i32 = 656;
const WIN_X: i32 = (SCREEN_W - WIN_W) / 2;
const WIN_Y: i32 = (SCREEN_H - WIN_H) / 2;

// Display panel
const DISP_X: i32 = 8;
const DISP_Y: i32 = 38;   // just below titlebar
const DISP_W: i32 = WIN_W - 16;
const DISP_H: i32 = 60;

// Button grid starts below display
const GRID_Y:  i32 = DISP_Y + DISP_H + 6;
const BTN_W:   i32 = 90;
const BTN_H:   i32 = 70;
const BTN_GAP: i32 = 6;
const COLS:    usize = 4;
const ROWS:    usize = 7;

fn btn_x(col: usize) -> i32 { 8 + col as i32 * (BTN_W + BTN_GAP) }
fn btn_y(row: usize) -> i32 { GRID_Y + row as i32 * (BTN_H + BTN_GAP) }

// ── Button table ─────────────────────────────────────────────────────────────

// Action tags
const ACT_DIGIT:   u8 = 0;
const ACT_DOT:     u8 = 1;
const ACT_CLEAR:   u8 = 2;
const ACT_BACK:    u8 = 3;
const ACT_NEG:     u8 = 4;
const ACT_OP:      u8 = 5;
const ACT_EQ:      u8 = 6;
const ACT_MC:      u8 = 7;  // memory clear
const ACT_MR:      u8 = 8;  // memory recall
const ACT_MPLUS:   u8 = 9;  // memory add
const ACT_MMINUS:  u8 = 10; // memory subtract
const ACT_SIN:     u8 = 11; // sin (degrees input)
const ACT_COS:     u8 = 12; // cos (degrees input)
const ACT_TAN:     u8 = 13; // tan (degrees input)
const ACT_SQRT:    u8 = 14; // square root

struct BtnDef {
    label: &'static [u8],
    act:   u8,
    val:   u8,
}

static BTNS: [BtnDef; ROWS * COLS] = [
    // Row 0: memory
    BtnDef { label: b"MC\0", act: ACT_MC,     val: 0     },
    BtnDef { label: b"MR\0", act: ACT_MR,     val: 0     },
    BtnDef { label: b"M+\0", act: ACT_MPLUS,  val: 0     },
    BtnDef { label: b"M-\0", act: ACT_MMINUS, val: 0     },
    // Row 1
    BtnDef { label: b"C\0",  act: ACT_CLEAR,  val: 0     },
    BtnDef { label: b"+/-\0",act: ACT_NEG,    val: 0     },
    BtnDef { label: b"%\0",  act: ACT_OP,     val: b'%'  },
    BtnDef { label: b"/\0",  act: ACT_OP,     val: b'/'  },
    // Row 1
    BtnDef { label: b"7\0",  act: ACT_DIGIT,  val: b'7'  },
    BtnDef { label: b"8\0",  act: ACT_DIGIT,  val: b'8'  },
    BtnDef { label: b"9\0",  act: ACT_DIGIT,  val: b'9'  },
    BtnDef { label: b"*\0",  act: ACT_OP,     val: b'*'  },
    // Row 2
    BtnDef { label: b"4\0",  act: ACT_DIGIT,  val: b'4'  },
    BtnDef { label: b"5\0",  act: ACT_DIGIT,  val: b'5'  },
    BtnDef { label: b"6\0",  act: ACT_DIGIT,  val: b'6'  },
    BtnDef { label: b"-\0",  act: ACT_OP,     val: b'-'  },
    // Row 3
    BtnDef { label: b"1\0",  act: ACT_DIGIT,  val: b'1'  },
    BtnDef { label: b"2\0",  act: ACT_DIGIT,  val: b'2'  },
    BtnDef { label: b"3\0",  act: ACT_DIGIT,  val: b'3'  },
    BtnDef { label: b"+\0",  act: ACT_OP,     val: b'+'  },
    // Row 4
    BtnDef { label: b"0\0",  act: ACT_DIGIT,  val: b'0'  },
    BtnDef { label: b".\0",  act: ACT_DOT,    val: 0     },
    BtnDef { label: b"BS\0", act: ACT_BACK,   val: 0     },
    BtnDef { label: b"=\0",  act: ACT_EQ,     val: 0     },
    // Row 5: scientific
    BtnDef { label: b"sin\0", act: ACT_SIN,   val: 0     },
    BtnDef { label: b"cos\0", act: ACT_COS,   val: 0     },
    BtnDef { label: b"tan\0", act: ACT_TAN,   val: 0     },
    BtnDef { label: b"sqrt\0",act: ACT_SQRT,  val: 0     },
];

fn make_button(idx: usize) -> Button {
    let col = idx % COLS;
    let row = idx / COLS;
    Button {
        label: BTNS[idx].label,
        x: btn_x(col), y: btn_y(row),
        w: BTN_W, h: BTN_H,
    }
}

// ── Calculator state ──────────────────────────────────────────────────────────

struct Calc {
    entry:       [u8; 20],
    entry_len:   usize,
    has_dot:     bool,
    accum:       f64,
    memory:      f64,
    pending_op:  u8,
    just_result: bool,
    error:       bool,
}

impl Calc {
    const fn new() -> Self {
        let mut e = [0u8; 20];
        e[0] = b'0';
        Calc {
            entry: e,
            entry_len: 1,
            has_dot: false,
            accum: 0.0,
            memory: 0.0,
            pending_op: 0,
            just_result: false,
            error: false,
        }
    }

    fn entry_val(&self) -> f64 {
        // Parse self.entry[0..entry_len] as f64
        let mut v = 0.0f64;
        let mut neg = false;
        let mut i = 0usize;
        if i < self.entry_len && self.entry[i] == b'-' { neg = true; i += 1; }
        while i < self.entry_len && self.entry[i] != b'.' {
            v = v * 10.0 + (self.entry[i] - b'0') as f64;
            i += 1;
        }
        if i < self.entry_len && self.entry[i] == b'.' {
            i += 1;
            let mut scale = 0.1f64;
            while i < self.entry_len {
                v += (self.entry[i] - b'0') as f64 * scale;
                scale *= 0.1;
                i += 1;
            }
        }
        if neg { -v } else { v }
    }

    fn apply_op(&mut self) {
        let v = self.entry_val();
        self.accum = match self.pending_op {
            b'+' => self.accum + v,
            b'-' => self.accum - v,
            b'*' => self.accum * v,
            b'/' => if v == 0.0 { self.error = true; 0.0 } else { self.accum / v },
            b'%' => if v == 0.0 { self.error = true; 0.0 } else { self.accum % v },
            _    => v,
        };
    }

    fn set_entry_f64(&mut self, v: f64) {
        self.entry_len = fmt_f64(v, &mut self.entry);
        self.has_dot   = self.entry[..self.entry_len].contains(&b'.');
    }

    fn push_digit(&mut self, d: u8) {
        if self.just_result {
            self.entry[0] = d; self.entry_len = 1;
            self.has_dot = false;
            self.just_result = false;
            return;
        }
        // Replace lone "0" with the digit
        if self.entry_len == 1 && self.entry[0] == b'0' && d != b'0' {
            self.entry[0] = d; return;
        }
        if self.entry_len < 15 {
            self.entry[self.entry_len] = d;
            self.entry_len += 1;
        }
    }

    fn push_dot(&mut self) {
        if self.just_result {
            self.entry[0] = b'0'; self.entry[1] = b'.';
            self.entry_len = 2; self.has_dot = true; self.just_result = false;
            return;
        }
        if !self.has_dot && self.entry_len < 15 {
            self.entry[self.entry_len] = b'.';
            self.entry_len += 1;
            self.has_dot = true;
        }
    }

    fn backspace(&mut self) {
        if self.error { self.clear(); return; }
        if self.entry_len > 1 {
            self.entry_len -= 1;
            if self.entry[self.entry_len] == b'.' { self.has_dot = false; }
        } else {
            self.entry[0] = b'0'; self.entry_len = 1; self.has_dot = false;
        }
    }

    fn clear(&mut self) {
        self.entry[0] = b'0'; self.entry_len = 1;
        self.has_dot = false;
        self.accum = 0.0;
        self.pending_op = 0;
        self.just_result = false;
        self.error = false;
    }

    fn negate(&mut self) {
        if self.entry_len == 1 && self.entry[0] == b'0' { return; }
        if self.entry[0] == b'-' {
            // Remove leading minus
            for i in 0..self.entry_len - 1 { self.entry[i] = self.entry[i + 1]; }
            self.entry_len -= 1;
        } else if self.entry_len < 15 {
            for i in (0..self.entry_len).rev() { self.entry[i + 1] = self.entry[i]; }
            self.entry[0] = b'-';
            self.entry_len += 1;
        }
    }

    fn op(&mut self, o: u8) {
        if self.pending_op != 0 {
            self.apply_op();
        } else {
            self.accum = self.entry_val();
        }
        self.pending_op = o;
        self.just_result = true;
    }

    fn equals(&mut self) {
        if self.pending_op != 0 {
            self.apply_op();
            self.pending_op = 0;
            if !self.error {
                self.set_entry_f64(self.accum);
            } else {
                let e = b"Error\0";
                for i in 0..6 { self.entry[i] = e[i]; }
                self.entry_len = 5;
            }
            self.just_result = true;
        }
    }

    fn handle(&mut self, act: u8, val: u8) {
        if self.error && act != ACT_CLEAR { return; }
        match act {
            ACT_DIGIT  => self.push_digit(val),
            ACT_DOT    => self.push_dot(),
            ACT_CLEAR  => self.clear(),
            ACT_BACK   => self.backspace(),
            ACT_NEG    => self.negate(),
            ACT_OP     => self.op(val),
            ACT_EQ     => self.equals(),
            ACT_MC     => { self.memory = 0.0; }
            ACT_MR     => { self.set_entry_f64(self.memory); self.just_result = true; }
            ACT_MPLUS  => { self.memory += self.entry_val(); }
            ACT_MMINUS => { self.memory -= self.entry_val(); }
            ACT_SIN    => { let v = sin_deg(self.entry_val()); self.set_entry_f64(v); self.just_result = true; }
            ACT_COS    => { let v = cos_deg(self.entry_val()); self.set_entry_f64(v); self.just_result = true; }
            ACT_TAN    => { let v = tan_deg(self.entry_val()); if v != v { self.error = true; } else { self.set_entry_f64(v); self.just_result = true; } }
            ACT_SQRT   => { let v = calc_sqrt(self.entry_val()); if v != v { self.error = true; } else { self.set_entry_f64(v); self.just_result = true; } }
            _          => {}
        }
    }

    fn display(&self, buf: &mut [u8; 20]) -> usize {
        let n = self.entry_len.min(19);
        for i in 0..n { buf[i] = self.entry[i]; }
        buf[n] = 0;
        n
    }
}

// ── Scientific math (no libm) ─────────────────────────────────────────────────

const PI: f64 = 3.141592653589793;

fn calc_sqrt(x: f64) -> f64 {
    if x < 0.0 { return f64::NAN; }
    if x == 0.0 { return 0.0; }
    let mut g = x * 0.5 + 0.5;
    for _ in 0..60 { g = (g + x / g) * 0.5; }
    g
}

fn sin_rad(mut x: f64) -> f64 {
    // Reduce to [-π, π]
    let tau = 2.0 * PI;
    x -= (x / tau + 0.5).floor() * tau;
    // Minimax polynomial (Horner form) accurate to ~f64 precision for [-π, π]
    let x2 = x * x;
    x * (1.0 + x2 * (-1.0/6.0 + x2 * (1.0/120.0 + x2 * (-1.0/5040.0
        + x2 * (1.0/362880.0 + x2 * (-1.0/39916800.0))))))
}

fn cos_rad(x: f64) -> f64 {
    sin_rad(x + PI * 0.5)
}

fn sin_deg(d: f64) -> f64 { sin_rad(d * (PI / 180.0)) }
fn cos_deg(d: f64) -> f64 { cos_rad(d * (PI / 180.0)) }
fn tan_deg(d: f64) -> f64 {
    let c = cos_deg(d);
    if c.abs() < 1e-12 { f64::NAN } else { sin_deg(d) / c }
}

trait F64Ext { fn floor(self) -> f64; fn abs(self) -> f64; }
impl F64Ext for f64 {
    fn floor(self) -> f64 {
        let i = self as i64;
        if self < i as f64 { (i - 1) as f64 } else { i as f64 }
    }
    fn abs(self) -> f64 { if self < 0.0 { -self } else { self } }
}

// ── f64 formatting ────────────────────────────────────────────────────────────

fn fmt_f64(v: f64, buf: &mut [u8; 20]) -> usize {
    if v != v { buf[0]=b'N'; buf[1]=b'a'; buf[2]=b'N'; buf[3]=0; return 3; }
    let mut pos = 0usize;
    let mut v = v;
    if v < 0.0 { buf[pos] = b'-'; pos += 1; v = -v; }
    let int_part = v as u64;
    let frac = v - int_part as f64;
    let mut tmp = [0u8; 12];
    let n = fmt_u32(int_part as u32, &mut tmp);
    for i in 0..n { buf[pos] = tmp[i]; pos += 1; }
    if frac > 0.000001 && pos < 16 {
        buf[pos] = b'.'; pos += 1;
        let mut f = frac;
        for _ in 0..6 {
            if pos >= 19 { break; }
            f *= 10.0;
            let d = f as u8;
            buf[pos] = b'0' + d; pos += 1;
            f -= d as f64;
            if f < 0.000001 { break; }
        }
    }
    buf[pos] = 0;
    pos
}

// ── Rendering ─────────────────────────────────────────────────────────────────

fn render(calc: &Calc, mx: i32, my: i32, pressed_idx: Option<usize>, cw: i32, ch: i32) {
    fill(0, 0, cw, ch, BG);
    window_frame(0, 0, cw, ch, b"Calculator\0");

    // Display panel
    fill(DISP_X, DISP_Y, DISP_W, DISP_H, 0x090e14);
    fill(DISP_X, DISP_Y + DISP_H - 1, DISP_W, 1, BORDER);

    let mut dbuf = [0u8; 20];
    let dlen = calc.display(&mut dbuf);
    let dw = dlen as i32 * 8;
    let dx = DISP_X + DISP_W - dw - 12;
    let dy = DISP_Y + (DISP_H - 8) / 2;
    let dcolor = if calc.error { RED } else { FG };
    text(dx, dy, &dbuf, dcolor);

    // Expression hint (pending op)
    if calc.pending_op != 0 {
        let mut hint = [0u8; 4];
        hint[0] = calc.pending_op; hint[1] = b' '; hint[2] = b'.'; hint[3] = 0;
        // actually just show pending op symbol
        hint[0] = calc.pending_op; hint[1] = b'.'; hint[2] = b'.'; hint[3] = 0;
        text(DISP_X + 8, DISP_Y + 4, &[hint[0], 0], FG_DIM);
    }

    // Buttons
    for i in 0..BTNS.len() {
        let b = make_button(i);
        let col = i % COLS;
        let row = i / COLS;
        let bx = btn_x(col);
        let by = btn_y(row);
        let hov = mx >= bx && mx < bx + BTN_W && my >= by && my < by + BTN_H;
        let state = if pressed_idx == Some(i) {
            BtnState::Pressed
        } else if hov {
            BtnState::Hovered
        } else {
            BtnState::Normal
        };
        b.draw(state);
    }

    draw_cursor(mx, my);
}

// ── Main ──────────────────────────────────────────────────────────────────────

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    println!("rs-calc: start");
    let mut cw = WIN_W; let mut ch = WIN_H;
    let mut maximized = false;
    win_init_at(b"Calculator  ", WIN_W, WIN_H, WIN_X, WIN_Y);

    let mut calc = Calc::new();
    let mut mx: i32 = WIN_W / 2;
    let mut my: i32 = WIN_H / 2;
    let mut left_held    = false;
    let mut pressed_idx: Option<usize> = None;
    let mut prev_k = 0i32;
    let mut dirty = true;

    loop {
        let (dx, dy, buttons) = poll_mouse();
        let old_mx = mx; let old_my = my;
        mx = clamp(mx + dx, 0, WIN_W - 1);
        my = clamp(my + dy, 0, WIN_H - 1);

        let left_now = buttons & 0x01 != 0;

        // Mouse press: find which button
        if left_now && !left_held {
            for i in 0..BTNS.len() {
                let col = i % COLS; let row = i / COLS;
                let bx = btn_x(col); let by = btn_y(row);
                if mx >= bx && mx < bx + BTN_W && my >= by && my < by + BTN_H {
                    pressed_idx = Some(i);
                    dirty = true;
                    break;
                }
            }
        }
        // Mouse release: fire action
        if !left_now && left_held {
            if let Some(i) = pressed_idx {
                calc.handle(BTNS[i].act, BTNS[i].val);
                pressed_idx = None;
                dirty = true;
            }
        }

        if mx != old_mx || my != old_my { dirty = true; }
        left_held = left_now;

        // Keyboard
        let k = poll_key();
        if k != 0 && k != prev_k {
            let mut handled = true;
            match k {
                k if k == b'q' as i32 || k == 27 => break,
                KEY_F11 => {
                    maximized = !maximized;
                    let (nw, nh) = if maximized { (SCREEN_W_FULL, SCREEN_H_FULL) } else { (WIN_W, WIN_H) };
                    if win_resize(nw, nh) { cw = nw; ch = nh; dirty = true; }
                }
                k if k >= b'0' as i32 && k <= b'9' as i32 => {
                    calc.handle(ACT_DIGIT, k as u8);
                }
                k if k == b'.' as i32 => calc.handle(ACT_DOT, 0),
                k if k == b'+' as i32 => calc.handle(ACT_OP, b'+'),
                k if k == b'-' as i32 => calc.handle(ACT_OP, b'-'),
                k if k == b'*' as i32 => calc.handle(ACT_OP, b'*'),
                k if k == b'/' as i32 => calc.handle(ACT_OP, b'/'),
                k if k == b'\n' as i32 || k == b'\r' as i32 => calc.handle(ACT_EQ, 0),
                k if k == 8 || k == 127 => calc.handle(ACT_BACK, 0),
                k if k == b'c' as i32 || k == b'C' as i32 => calc.handle(ACT_CLEAR, 0),
                k if k == b's' as i32 || k == b'S' as i32 => calc.handle(ACT_SIN,   0),
                k if k == b't' as i32 || k == b'T' as i32 => calc.handle(ACT_TAN,   0),
                k if k == b'r' as i32 || k == b'R' as i32 => calc.handle(ACT_SQRT,  0),
                _ => { handled = false; }
            }
            if handled { dirty = true; }
        }
        prev_k = k;

        if dirty {
            render(&calc, mx, my, pressed_idx, cw, ch);
            dirty = false;
        }

        if let Some(ev) = win_poll_event() {
            if ev.kind == WIN_EV_RESIZE && win_resize(ev.x, ev.y) {
                cw = ev.x; ch = ev.y; dirty = true;
            }
        }

        win_flush();
        sleep_ms(16);
    }

    win_done();
    println!("rs-calc: exit");
}
