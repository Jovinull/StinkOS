//! rs-calc: pocket calculator for StinkOS.
//!
//! Works the way a pocket calculator does: operations are evaluated left to
//! right as they are entered (2 + 3 * 4 = gives 20), '=' pressed again
//! repeats the last operation, and '%' turns the entry into a percentage
//! (of the left operand after + or -, of 1 otherwise).
//!
//! Mouse: every key is a button. Keyboard: 0-9 . + - * / %, Enter or '='
//! for equals, Backspace deletes the last digit typed, C clears, Esc clears
//! when there is something to clear and closes the window otherwise, Q
//! closes. S, T and R are sin, tan and sqrt. Angles are in degrees.
//!
//! Numbers are f64. The display shows 12 significant digits and switches to
//! exponent notation when a value does not fit. No libm: sin/cos use Taylor
//! series after reducing the angle exactly in degrees, sqrt is Newton's
//! iteration.

#![no_std]
#![no_main]

use libstink::println;
use libui::*;

// ── Numbers ─────────────────────────────────────────────────────────────────

/// Digits that can be typed, and significant digits shown for a result.
const MAX_DIGITS: usize = 12;
/// Room for the longest display text: "-0.000000000" + 12 digits.
const TEXT_CAP: usize = 32;

const POW10: [f64; MAX_DIGITS + 1] =
    [1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12];
/// 10^(2^i), to scale any f64 into [1, 10) in at most nine steps.
const POW10_BIN: [f64; 9] = [1e1, 1e2, 1e4, 1e8, 1e16, 1e32, 1e64, 1e128, 1e256];

fn put(buf: &mut [u8; TEXT_CAP], pos: &mut usize, c: u8) {
    if *pos < TEXT_CAP {
        buf[*pos] = c;
        *pos += 1;
    }
}

/// Write a finite `v` rounded to 12 significant digits, trailing zeros
/// removed. Plain decimal when the exponent is between -9 and 11
/// ("0.000000001" .. "999999999999"), otherwise "d.ddde±x". Returns the
/// length; the longest output is 23 characters.
fn fmt_f64(v: f64, buf: &mut [u8; TEXT_CAP]) -> usize {
    if v == 0.0 || !v.is_finite() {
        buf[0] = b'0';
        return 1;
    }

    // x * 10^e == |v| with x in [1, 10).
    let mut x = v.abs();
    let mut e: i32 = 0;
    if x >= 10.0 {
        for i in (0..POW10_BIN.len()).rev() {
            if x >= POW10_BIN[i] {
                x /= POW10_BIN[i];
                e += 1 << i;
            }
        }
    } else if x < 1.0 {
        for i in (0..POW10_BIN.len()).rev() {
            if x * POW10_BIN[i] < 10.0 {
                x *= POW10_BIN[i];
                e -= 1 << i;
            }
        }
    }

    // The 12 digits as one integer in [10^11, 10^12).
    let mut m = (x * POW10[MAX_DIGITS - 1] + 0.5) as u64;
    if m >= 1_000_000_000_000 {
        m /= 10;                           // 9.999.. rounded up to the next power of ten
        e += 1;
    }
    while m != 0 && m < 100_000_000_000 {
        m *= 10;                           // scaling left x a hair under 1
        e -= 1;
    }

    let mut d = [b'0'; MAX_DIGITS];
    for i in (0..MAX_DIGITS).rev() {
        d[i] = b'0' + (m % 10) as u8;
        m /= 10;
    }
    let mut nd = MAX_DIGITS;               // significant digits to print
    while nd > 1 && d[nd - 1] == b'0' {
        nd -= 1;
    }

    let mut p = 0usize;
    if v < 0.0 {
        put(buf, &mut p, b'-');
    }
    if (0..MAX_DIGITS as i32).contains(&e) {
        // d[0..=e] is the integer part.
        let int_len = e as usize + 1;
        for i in 0..int_len {
            put(buf, &mut p, d[i]);
        }
        if nd > int_len {
            put(buf, &mut p, b'.');
            for i in int_len..nd {
                put(buf, &mut p, d[i]);
            }
        }
    } else if (-9..0).contains(&e) {
        put(buf, &mut p, b'0');
        put(buf, &mut p, b'.');
        for _ in 0..(-e - 1) {
            put(buf, &mut p, b'0');
        }
        for i in 0..nd {
            put(buf, &mut p, d[i]);
        }
    } else {
        put(buf, &mut p, d[0]);
        if nd > 1 {
            put(buf, &mut p, b'.');
            for i in 1..nd {
                put(buf, &mut p, d[i]);
            }
        }
        put(buf, &mut p, b'e');
        let mut mag = e;
        if mag < 0 {
            put(buf, &mut p, b'-');
            mag = -mag;
        }
        if mag >= 100 {
            put(buf, &mut p, b'0' + (mag / 100) as u8);
        }
        if mag >= 10 {
            put(buf, &mut p, b'0' + (mag / 10 % 10) as u8);
        }
        put(buf, &mut p, b'0' + (mag % 10) as u8);
    }
    p
}

/// Value of a typed entry: an optional '-', digits, an optional '.' and
/// more digits, at most MAX_DIGITS digits in all. The digits are read as
/// one integer and divided by a power of ten once, so the result is the
/// f64 nearest to what was typed.
fn parse_entry(s: &[u8]) -> f64 {
    let mut mant: u64 = 0;
    let mut frac = 0usize;
    let mut seen_dot = false;
    let mut neg = false;
    for &c in s {
        match c {
            b'-' => neg = true,
            b'.' => seen_dot = true,
            b'0'..=b'9' => {
                mant = mant * 10 + (c - b'0') as u64;
                if seen_dot {
                    frac += 1;
                }
            }
            _ => {}
        }
    }
    let v = mant as f64 / POW10[if frac > MAX_DIGITS { MAX_DIGITS } else { frac }];
    if neg && v != 0.0 { -v } else { v }
}

// ── Functions (no libm) ─────────────────────────────────────────────────────

const PI: f64 = 3.141592653589793;
const RAD_PER_DEG: f64 = PI / 180.0;

/// Largest integer not above `x`, for |x| below 2^63.
fn floor(x: f64) -> f64 {
    let f = (x as i64) as f64;
    if x < f { f - 1.0 } else { f }
}

/// sin(x) for x in [0, pi/4]; the first dropped term is below 1e-19.
fn sin_small(x: f64) -> f64 {
    let x2 = x * x;
    x * (1.0 + x2 * (-1.0 / 6.0 + x2 * (1.0 / 120.0 + x2 * (-1.0 / 5040.0
        + x2 * (1.0 / 362880.0 + x2 * (-1.0 / 39916800.0 + x2 * (1.0 / 6227020800.0
        + x2 * (-1.0 / 1307674368000.0 + x2 * (1.0 / 355687428096000.0)))))))))
}

/// cos(x) for x in [0, pi/4]; the first dropped term is below 1e-17.
fn cos_small(x: f64) -> f64 {
    let x2 = x * x;
    1.0 + x2 * (-1.0 / 2.0 + x2 * (1.0 / 24.0 + x2 * (-1.0 / 720.0
        + x2 * (1.0 / 40320.0 + x2 * (-1.0 / 3628800.0 + x2 * (1.0 / 479001600.0
        + x2 * (-1.0 / 87178291200.0 + x2 * (1.0 / 20922789888000.0))))))))
}

/// `d` degrees reduced to [0, 360), or None when `d` is too large for its
/// fraction of a turn to mean anything.
fn reduce_deg(d: f64) -> Option<f64> {
    if !(d.abs() < 1e15) {
        return None;
    }
    let mut a = d - floor(d / 360.0) * 360.0;
    if a < 0.0 {
        a += 360.0;
    }
    if a >= 360.0 {
        a -= 360.0;
    }
    Some(a)
}

/// sin of `a` degrees, `a` in [0, 360]. The angle is folded into the first
/// half-quadrant while it is still in degrees, so multiples of 90 give
/// exactly 0 and 1.
fn sin_turn(a: f64) -> f64 {
    let mut a = a;
    let mut sign = 1.0;
    if a >= 180.0 {
        a -= 180.0;
        sign = -1.0;
    }
    if a > 90.0 {
        a = 180.0 - a;
    }
    let r = if a <= 45.0 { sin_small(a * RAD_PER_DEG) } else { cos_small((90.0 - a) * RAD_PER_DEG) };
    sign * r
}

fn sin_deg(d: f64) -> f64 {
    match reduce_deg(d) {
        Some(a) => sin_turn(a),
        None => f64::NAN,
    }
}

fn cos_deg(d: f64) -> f64 {
    match reduce_deg(d) {
        // cos(a) = sin(a + 90); a + 90 stays within [90, 450).
        Some(a) => sin_turn(if a + 90.0 >= 360.0 { a - 270.0 } else { a + 90.0 }),
        None => f64::NAN,
    }
}

/// NaN where the tangent is undefined (90, 270, ...).
fn tan_deg(d: f64) -> f64 {
    let c = cos_deg(d);
    if c.is_nan() || c == 0.0 { f64::NAN } else { sin_deg(d) / c }
}

/// NaN for a negative argument.
fn sqrt(x: f64) -> f64 {
    if x.is_nan() || x < 0.0 {
        return f64::NAN;
    }
    if x == 0.0 {
        return 0.0;
    }
    // Halving the binary exponent gives a first guess within a factor of
    // 1.5 of the root for any magnitude; Newton's iteration does the rest.
    let mut g = f64::from_bits((x.to_bits() >> 1) + 0x1FF8_0000_0000_0000);
    for _ in 0..64 {
        let n = 0.5 * (g + x / g);
        if n == g {
            break;
        }
        g = n;
    }
    g
}

// ── Calculator ──────────────────────────────────────────────────────────────

const ERR_NONE:     u8 = 0;
const ERR_DIV0:     u8 = 1;
const ERR_INVALID:  u8 = 2;
const ERR_OVERFLOW: u8 = 3;

#[derive(Clone, Copy, PartialEq, Eq)]
enum Key {
    Digit(u8),          // b'0'..=b'9'
    Dot,
    Clear,
    Back,
    Negate,
    Percent,
    Op(u8),             // b'+', b'-', b'*', b'/'
    Equals,
    MemClear,
    MemRecall,
    MemAdd,
    MemSub,
    Sin,
    Cos,
    Tan,
    Sqrt,
}

/// `a op b`, or the error it raises.
fn apply(op: u8, a: f64, b: f64) -> Result<f64, u8> {
    let r = match op {
        b'+' | b'-' => {
            let r = if op == b'+' { a + b } else { a - b };
            // A sum 13 orders of magnitude below its terms is binary
            // rounding noise (0.1 + 0.2 - 0.3), not a digit anyone typed.
            let big = if a.abs() > b.abs() { a.abs() } else { b.abs() };
            if r.abs() < big * 1e-13 { 0.0 } else { r }
        }
        b'*' => a * b,
        b'/' => {
            if b == 0.0 {
                return Err(ERR_DIV0);
            }
            a / b
        }
        _ => b,
    };
    if r.is_finite() { Ok(r) } else { Err(ERR_OVERFLOW) }
}

struct Calc {
    /// The number on the display, as text and as a value. While a number
    /// is being typed the value is parsed from the text; a result keeps
    /// its full f64 precision and the text is its 12-digit rendering.
    text:    [u8; TEXT_CAP],
    len:     usize,
    value:   f64,
    /// The display is a number being typed: digits append to it and
    /// Backspace edits it. Otherwise the next digit starts a new number.
    typing:  bool,
    /// Left operand and operator waiting for their right operand (0: none).
    acc:     f64,
    pending: u8,
    /// The display holds the right operand of `pending` (typed, recalled
    /// or computed since the operator was pressed).
    operand: bool,
    /// The last completed operation, repeated by '=' (0: none).
    rep_op:  u8,
    rep_val: f64,
    memory:  f64,
    error:   u8,
}

impl Calc {
    fn new() -> Calc {
        let mut c = Calc {
            text: [0; TEXT_CAP], len: 0, value: 0.0, typing: false,
            acc: 0.0, pending: 0, operand: false, rep_op: 0, rep_val: 0.0,
            memory: 0.0, error: ERR_NONE,
        };
        c.clear();
        c
    }

    /// Back to "0" with nothing pending. The memory is kept.
    fn clear(&mut self) {
        self.text[0] = b'0';
        self.len = 1;
        self.value = 0.0;
        self.typing = false;
        self.acc = 0.0;
        self.pending = 0;
        self.operand = false;
        self.rep_op = 0;
        self.rep_val = 0.0;
        self.error = ERR_NONE;
    }

    /// Nothing for Clear to do: the display reads 0 and no operation waits.
    fn is_clear(&self) -> bool {
        self.error == ERR_NONE && self.pending == 0 && self.len == 1 && self.text[0] == b'0'
    }

    fn fail(&mut self, code: u8) {
        self.clear();
        self.error = code;
    }

    /// Put a computed value on the display; the next digit starts a new
    /// number. A value that is not a number or not finite is an error.
    fn show(&mut self, v: f64) {
        if v.is_nan() {
            self.fail(ERR_INVALID);
        } else if !v.is_finite() {
            self.fail(ERR_OVERFLOW);
        } else {
            self.value = if v == 0.0 { 0.0 } else { v };     // no negative zero
            self.len = fmt_f64(self.value, &mut self.text);
            self.typing = false;
        }
    }

    /// As `show`, and the value counts as the right operand of a pending
    /// operator.
    fn show_operand(&mut self, v: f64) {
        self.show(v);
        if self.error == ERR_NONE {
            self.operand = true;
        }
    }

    fn entry(&self) -> &[u8] {
        &self.text[..self.len]
    }

    fn digit_count(&self) -> usize {
        self.entry().iter().filter(|c| c.is_ascii_digit()).count()
    }

    /// Start typing a new number unless one is being typed already.
    fn begin_entry(&mut self) {
        if !self.typing {
            self.text[0] = b'0';
            self.len = 1;
            self.typing = true;
        }
    }

    /// The text was edited: recompute the value.
    fn reparse(&mut self) {
        self.value = parse_entry(&self.text[..self.len]);
        self.operand = true;
    }

    fn digit(&mut self, d: u8) {
        self.begin_entry();
        if self.len == 1 && self.text[0] == b'0' {
            self.text[0] = d;                          // no leading zeros
        } else if self.digit_count() < MAX_DIGITS {
            self.text[self.len] = d;
            self.len += 1;
        }
        self.reparse();
    }

    fn dot(&mut self) {
        self.begin_entry();
        // One point per number, and only while a digit can still follow it.
        if !self.entry().contains(&b'.') && self.digit_count() < MAX_DIGITS {
            self.text[self.len] = b'.';
            self.len += 1;
        }
        self.reparse();
    }

    /// Delete the last character typed. A result is not editable.
    fn backspace(&mut self) {
        if !self.typing {
            return;
        }
        self.len -= 1;
        if matches!(self.entry(), b"" | b"-" | b"-0") {
            self.text[0] = b'0';
            self.len = 1;
        }
        self.reparse();
    }

    fn negate(&mut self) {
        if !self.typing {
            if self.value != 0.0 {
                self.show_operand(-self.value);
            }
            return;
        }
        // Flip the sign in the text, so "0.0" can become "-0.0" on the
        // way to "-0.05". A bare "0" has no sign.
        if self.len == 1 && self.text[0] == b'0' {
            return;
        }
        if self.text[0] == b'-' {
            self.text.copy_within(1..self.len, 0);
            self.len -= 1;
        } else {
            self.text.copy_within(0..self.len, 1);
            self.text[0] = b'-';
            self.len += 1;
        }
        self.reparse();
    }

    /// + - * /: finish the pending operation if its right operand is there,
    /// then wait for the next operand. Pressed straight after another
    /// operator, it replaces that operator.
    fn operator(&mut self, op: u8) {
        if self.pending != 0 && self.operand {
            match apply(self.pending, self.acc, self.value) {
                Ok(r) => self.show(r),
                Err(e) => {
                    self.fail(e);
                    return;
                }
            }
        } else {
            self.show(self.value);
        }
        self.acc = self.value;
        self.pending = op;
        self.operand = false;
    }

    /// '=': finish the pending operation (with the displayed value as the
    /// right operand when none was entered), or repeat the last one.
    fn equals(&mut self) {
        let (op, a, b) = if self.pending != 0 {
            (self.pending, self.acc, self.value)
        } else if self.rep_op != 0 {
            (self.rep_op, self.value, self.rep_val)
        } else {
            self.show(self.value);
            return;
        };
        self.pending = 0;
        self.operand = false;
        match apply(op, a, b) {
            Ok(r) => {
                self.rep_op = op;
                self.rep_val = b;
                self.show(r);
            }
            Err(e) => self.fail(e),
        }
    }

    /// '%': after + or - the entry becomes that percentage of the left
    /// operand (200 + 10 % = 220); otherwise it is divided by 100.
    fn percent(&mut self) {
        let v = if self.pending == b'+' || self.pending == b'-' {
            self.acc * self.value / 100.0
        } else {
            self.value / 100.0
        };
        self.show_operand(v);
    }

    /// M+ / M-: add the displayed value to the memory and end the entry.
    fn mem_add(&mut self, sign: f64) {
        let m = self.memory + sign * self.value;
        if m.is_finite() {
            self.memory = m;
            self.show(self.value);
        } else {
            self.fail(ERR_OVERFLOW);
        }
    }

    fn press(&mut self, key: Key) {
        // After an error only clearing is accepted.
        if self.error != ERR_NONE {
            if key == Key::Clear || key == Key::Back {
                self.clear();
            }
            return;
        }
        match key {
            Key::Digit(d)  => self.digit(d),
            Key::Dot       => self.dot(),
            Key::Clear     => self.clear(),
            Key::Back      => self.backspace(),
            Key::Negate    => self.negate(),
            Key::Percent   => self.percent(),
            Key::Op(op)    => self.operator(op),
            Key::Equals    => self.equals(),
            Key::MemClear  => self.memory = 0.0,
            Key::MemRecall => self.show_operand(self.memory),
            Key::MemAdd    => self.mem_add(1.0),
            Key::MemSub    => self.mem_add(-1.0),
            Key::Sin       => self.show_operand(sin_deg(self.value)),
            Key::Cos       => self.show_operand(cos_deg(self.value)),
            Key::Tan       => self.show_operand(tan_deg(self.value)),
            Key::Sqrt      => self.show_operand(sqrt(self.value)),
        }
    }

    /// The main line of the display.
    fn display(&self) -> &[u8] {
        match self.error {
            ERR_NONE     => self.entry(),
            ERR_DIV0     => b"Cannot divide by zero",
            ERR_INVALID  => b"Invalid input",
            _            => b"Overflow",
        }
    }
}

// ── Layout ──────────────────────────────────────────────────────────────────

const COLS: usize = 4;
const ROWS: usize = 7;
const KEY_W: i32 = 64;

const WIN_W:  i32 = 2 * PAD + COLS as i32 * KEY_W + (COLS as i32 - 1) * GAP;
const DISP_H: i32 = 52;
const GRID_Y: i32 = PAD + DISP_H + GAP;
const WIN_H:  i32 = GRID_Y + ROWS as i32 * BTN_H + (ROWS as i32 - 1) * GAP + GAP + STATUS_H;

struct KeyDef {
    label: &'static [u8],
    key:   Key,
}

const fn k(label: &'static [u8], key: Key) -> KeyDef {
    KeyDef { label, key }
}

static KEYS: [KeyDef; ROWS * COLS] = [
    k(b"MC\0", Key::MemClear), k(b"MR\0", Key::MemRecall), k(b"M+\0", Key::MemAdd),   k(b"M-\0", Key::MemSub),
    k(b"C\0", Key::Clear),     k(b"+/-\0", Key::Negate),   k(b"%\0", Key::Percent),   k(b"/\0", Key::Op(b'/')),
    k(b"7\0", Key::Digit(b'7')), k(b"8\0", Key::Digit(b'8')), k(b"9\0", Key::Digit(b'9')), k(b"*\0", Key::Op(b'*')),
    k(b"4\0", Key::Digit(b'4')), k(b"5\0", Key::Digit(b'5')), k(b"6\0", Key::Digit(b'6')), k(b"-\0", Key::Op(b'-')),
    k(b"1\0", Key::Digit(b'1')), k(b"2\0", Key::Digit(b'2')), k(b"3\0", Key::Digit(b'3')), k(b"+\0", Key::Op(b'+')),
    k(b"0\0", Key::Digit(b'0')), k(b".\0", Key::Dot),         k(b"BS\0", Key::Back),       k(b"=\0", Key::Equals),
    k(b"sin\0", Key::Sin),     k(b"cos\0", Key::Cos),      k(b"tan\0", Key::Tan),     k(b"sqrt\0", Key::Sqrt),
];

fn key_rect(i: usize) -> Rect {
    let col = (i % COLS) as i32;
    let row = (i / COLS) as i32;
    Rect::new(PAD + col * (KEY_W + GAP), GRID_Y + row * (BTN_H + GAP), KEY_W, BTN_H)
}

fn key_at(x: i32, y: i32) -> Option<usize> {
    (0..KEYS.len()).find(|&i| key_rect(i).contains(x, y))
}

/// What a keyboard character does, if anything.
fn key_for(ch: u8) -> Option<Key> {
    Some(match ch {
        b'0'..=b'9' => Key::Digit(ch),
        b'.' => Key::Dot,
        b'+' | b'-' | b'*' | b'/' => Key::Op(ch),
        b'%' => Key::Percent,
        b'=' | KEY_ENTER | b'\r' => Key::Equals,
        KEY_BACKSPACE | 127 => Key::Back,
        b'c' | b'C' => Key::Clear,
        b's' | b'S' => Key::Sin,
        b't' | b'T' => Key::Tan,
        b'r' | b'R' => Key::Sqrt,
        _ => return None,
    })
}

// ── Drawing ─────────────────────────────────────────────────────────────────

fn draw(win: &Window, calc: &Calc, hot: Option<usize>, pressed: Option<usize>) {
    win.clear(BG);

    // Display: a small status line (memory flag, pending operation) above
    // the number, both right-aligned.
    let d = Rect::new(PAD, PAD, win.w() - 2 * PAD, DISP_H);
    win.fill_rect(d, SURFACE);
    win.rect(d, BORDER);
    let right = d.x + d.w - GAP;
    let top = d.y + 6;
    if calc.memory != 0.0 {
        win.text(d.x + GAP, top, b"M", ACCENT);
    }
    if calc.error != ERR_NONE {
        win.text_right(right, top, b"C: clear", FG_DIM);
    } else if calc.pending != 0 {
        let mut num = [0u8; TEXT_CAP];
        let n = fmt_f64(calc.acc, &mut num);
        let mut line = [0u8; TEXT_CAP + 2];
        line[..n].copy_from_slice(&num[..n]);
        line[n] = b' ';
        line[n + 1] = calc.pending;
        win.text_right(right, top, &line[..n + 2], FG_DIM);
    }
    win.text_right(right, top + FONT_H + 6, calc.display(),
                   if calc.error != ERR_NONE { RED } else { FG });

    for (i, def) in KEYS.iter().enumerate() {
        // The key the button went down on shows pressed while the pointer
        // is still over it; nothing else lights up during a press.
        let state = if pressed == Some(i) && hot == Some(i) { BtnState::Pressed }
                    else if pressed.is_none() && hot == Some(i) { BtnState::Hover }
                    else { BtnState::Normal };
        Button::new(key_rect(i), def.label).draw(win, state);
    }

    status_bar(win, b"Esc: clear, then close  Enter: =\0");
}

// ── Main ────────────────────────────────────────────────────────────────────

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    println!("rs-calc: start");
    let Some(mut win) = Window::open(b"Calculator\0", WIN_W, WIN_H, 0) else {
        println!("rs-calc: could not open a window");
        quit(1);
    };

    let mut calc = Calc::new();
    let mut hot: Option<usize> = None;       // key under the pointer
    let mut pressed: Option<usize> = None;   // key the left button went down on
    let mut dirty = true;

    loop {
        if dirty {
            draw(&win, &calc, hot, pressed);
            win.flush();
            dirty = false;
        }

        // Nothing here changes on its own: sleep until there is input.
        let Some(ev) = win.wait(60_000) else { continue };
        match ev {
            Event::Close => break,
            Event::Mouse { x, y, buttons, .. } => {
                let over = key_at(x, y);
                if ev.left_press().is_some() {
                    pressed = over;
                    dirty = true;
                } else if ev.left_release().is_some() {
                    // A click counts when press and release land on the same key.
                    if let Some(i) = pressed {
                        if over == Some(i) {
                            calc.press(KEYS[i].key);
                        }
                    }
                    pressed = None;
                    dirty = true;
                } else if buttons & 1 == 0 && pressed.is_some() {
                    pressed = None;              // the release event was lost
                    dirty = true;
                }
                if over != hot {
                    hot = over;
                    dirty = true;
                }
            }
            Event::Focus(false) => {
                // No more pointer events until the window is used again.
                if hot.is_some() || pressed.is_some() {
                    hot = None;
                    pressed = None;
                    dirty = true;
                }
            }
            Event::Key(KEY_ESC) => {
                if calc.is_clear() {
                    break;
                }
                calc.clear();
                dirty = true;
            }
            Event::Key(b'q') | Event::Key(b'Q') => break,
            Event::Key(ch) => {
                if let Some(key) = key_for(ch) {
                    calc.press(key);
                    dirty = true;
                }
            }
            _ => {}
        }
    }

    win.close();
    println!("rs-calc: exit");
}
