//! Pure-Rust JSON parser + pretty printer for StinkOS userland.
//!
//! Reads a JSON file from StinkFS (hardcoded for now: "TEST.JSON"),
//! parses it into a `Value` tree, then pretty-prints with 2-space
//! indentation back out via println!. Validates round-trip parsing
//! for nested objects/arrays, string escapes, and numeric edges.
//!
//! Output keys for tools/smoke-rs-json.py:
//!   "json: read N bytes"
//!   "json: parsed"
//!   "json: pretty start"
//!   "json: pretty end"
//!   "json: PASS"
//!
//! Zero external crates -- StinkOS rule. Recursive descent. Stack
//! depth capped to 64 so a malicious input can't overflow our 4 KiB
//! kernel-side user stack.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;
use alloc::string::{String, ToString};
use alloc::vec::Vec;
use core::alloc::{GlobalAlloc, Layout};
use libstink::{eprintln, exit, println};

extern "C" {
    fn malloc(n: usize) -> *mut u8;
    fn free(p: *mut u8);
    fn sys_fread(name: *const u8, buf: *mut u8, max: u32) -> i32;
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

// ---------- Value ----------

pub enum Value {
    Null,
    Bool(bool),
    Number(f64),
    String(String),
    Array(Vec<Value>),
    // Object preserves insertion order via Vec<(key, value)>; spec
    // doesn't require ordered keys but pretty-printing identical
    // input → identical output is the round-trip property our
    // smoke test relies on.
    Object(Vec<(String, Value)>),
}

// ---------- Parser ----------

const MAX_DEPTH: u32 = 64;

pub struct ParseError {
    pub msg: &'static str,
    pub pos: usize,
}

struct Parser<'a> {
    src: &'a [u8],
    pos: usize,
    depth: u32,
}

impl<'a> Parser<'a> {
    fn new(src: &'a [u8]) -> Self {
        Self { src, pos: 0, depth: 0 }
    }

    fn err(&self, msg: &'static str) -> ParseError {
        ParseError { msg, pos: self.pos }
    }

    fn peek(&self) -> Option<u8> {
        self.src.get(self.pos).copied()
    }

    fn bump(&mut self) -> Option<u8> {
        let c = self.peek()?;
        self.pos += 1;
        Some(c)
    }

    fn skip_ws(&mut self) {
        while let Some(c) = self.peek() {
            if c == b' ' || c == b'\t' || c == b'\n' || c == b'\r' {
                self.pos += 1;
            } else {
                break;
            }
        }
    }

    fn expect(&mut self, want: u8) -> Result<(), ParseError> {
        if self.bump() != Some(want) {
            self.pos -= 1;
            return Err(self.err("expected literal"));
        }
        Ok(())
    }

    fn parse_value(&mut self) -> Result<Value, ParseError> {
        if self.depth >= MAX_DEPTH {
            return Err(self.err("nesting too deep"));
        }
        self.skip_ws();
        let c = self.peek().ok_or(self.err("unexpected eof"))?;
        match c {
            b'{' => { self.depth += 1; let v = self.parse_object()?; self.depth -= 1; Ok(v) }
            b'[' => { self.depth += 1; let v = self.parse_array()?;  self.depth -= 1; Ok(v) }
            b'"' => self.parse_string().map(Value::String),
            b't' | b'f' => self.parse_bool(),
            b'n' => self.parse_null(),
            b'-' | b'0'..=b'9' => self.parse_number(),
            _ => Err(self.err("unexpected byte")),
        }
    }

    fn parse_object(&mut self) -> Result<Value, ParseError> {
        self.expect(b'{')?;
        let mut entries: Vec<(String, Value)> = Vec::new();
        self.skip_ws();
        if self.peek() == Some(b'}') {
            self.pos += 1;
            return Ok(Value::Object(entries));
        }
        loop {
            self.skip_ws();
            let k = self.parse_string()?;
            self.skip_ws();
            self.expect(b':')?;
            let v = self.parse_value()?;
            entries.push((k, v));
            self.skip_ws();
            match self.bump() {
                Some(b',') => continue,
                Some(b'}') => return Ok(Value::Object(entries)),
                _ => return Err(self.err("expected , or }")),
            }
        }
    }

    fn parse_array(&mut self) -> Result<Value, ParseError> {
        self.expect(b'[')?;
        let mut items: Vec<Value> = Vec::new();
        self.skip_ws();
        if self.peek() == Some(b']') {
            self.pos += 1;
            return Ok(Value::Array(items));
        }
        loop {
            items.push(self.parse_value()?);
            self.skip_ws();
            match self.bump() {
                Some(b',') => continue,
                Some(b']') => return Ok(Value::Array(items)),
                _ => return Err(self.err("expected , or ]")),
            }
        }
    }

    fn parse_string(&mut self) -> Result<String, ParseError> {
        self.expect(b'"')?;
        let mut out = String::new();
        loop {
            let c = self.bump().ok_or(self.err("eof inside string"))?;
            match c {
                b'"' => return Ok(out),
                b'\\' => {
                    let esc = self.bump().ok_or(self.err("eof in escape"))?;
                    match esc {
                        b'"'  => out.push('"'),
                        b'\\' => out.push('\\'),
                        b'/'  => out.push('/'),
                        b'b'  => out.push('\x08'),
                        b'f'  => out.push('\x0C'),
                        b'n'  => out.push('\n'),
                        b'r'  => out.push('\r'),
                        b't'  => out.push('\t'),
                        b'u'  => {
                            let mut cp = 0u32;
                            for _ in 0..4 {
                                let h = self.bump().ok_or(self.err("eof in \\u escape"))?;
                                let d = match h {
                                    b'0'..=b'9' => (h - b'0') as u32,
                                    b'a'..=b'f' => (h - b'a' + 10) as u32,
                                    b'A'..=b'F' => (h - b'A' + 10) as u32,
                                    _ => return Err(self.err("bad hex digit in \\u")),
                                };
                                cp = (cp << 4) | d;
                            }
                            // Surrogate pair support intentionally omitted; treat
                            // any code point individually. Lone surrogates become
                            // replacement char.
                            let ch = char::from_u32(cp).unwrap_or('\u{FFFD}');
                            out.push(ch);
                        }
                        _ => return Err(self.err("bad escape")),
                    }
                }
                _ => out.push(c as char),
            }
        }
    }

    fn parse_bool(&mut self) -> Result<Value, ParseError> {
        if self.src[self.pos..].starts_with(b"true") {
            self.pos += 4;
            Ok(Value::Bool(true))
        } else if self.src[self.pos..].starts_with(b"false") {
            self.pos += 5;
            Ok(Value::Bool(false))
        } else {
            Err(self.err("expected true|false"))
        }
    }

    fn parse_null(&mut self) -> Result<Value, ParseError> {
        if self.src[self.pos..].starts_with(b"null") {
            self.pos += 4;
            Ok(Value::Null)
        } else {
            Err(self.err("expected null"))
        }
    }

    fn parse_number(&mut self) -> Result<Value, ParseError> {
        let start = self.pos;
        if self.peek() == Some(b'-') { self.pos += 1; }
        while let Some(c) = self.peek() {
            if matches!(c, b'0'..=b'9' | b'.' | b'e' | b'E' | b'+' | b'-') {
                self.pos += 1;
            } else {
                break;
            }
        }
        let text = core::str::from_utf8(&self.src[start..self.pos])
            .map_err(|_| self.err("number not utf-8"))?;
        let n: f64 = text.parse().map_err(|_| self.err("bad number"))?;
        Ok(Value::Number(n))
    }
}

pub fn parse(src: &str) -> Result<Value, ParseError> {
    let mut p = Parser::new(src.as_bytes());
    let v = p.parse_value()?;
    p.skip_ws();
    if p.pos != p.src.len() {
        return Err(p.err("trailing garbage"));
    }
    Ok(v)
}

// ---------- Pretty printer ----------

fn emit_indent(out: &mut String, n: u32) {
    for _ in 0..n { out.push_str("  "); }
}

fn emit_string(out: &mut String, s: &str) {
    out.push('"');
    for c in s.chars() {
        match c {
            '"'  => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '\x08' => out.push_str("\\b"),
            '\x0C' => out.push_str("\\f"),
            c if (c as u32) < 0x20 => {
                out.push_str(&format!("\\u{:04x}", c as u32));
            }
            c => out.push(c),
        }
    }
    out.push('"');
}

fn emit_number(out: &mut String, n: f64) {
    // Integers print without trailing ".0" so the round-trip on
    // {"id":1} stays {"id":1}, not {"id":1.0}.
    if n.is_finite() && n == (n as i64 as f64) {
        out.push_str(&(n as i64).to_string());
    } else {
        out.push_str(&format!("{}", n));
    }
}

pub fn pretty(v: &Value) -> String {
    let mut out = String::new();
    write_value(&mut out, v, 0);
    out
}

fn write_value(out: &mut String, v: &Value, depth: u32) {
    match v {
        Value::Null      => out.push_str("null"),
        Value::Bool(b)   => out.push_str(if *b { "true" } else { "false" }),
        Value::Number(n) => emit_number(out, *n),
        Value::String(s) => emit_string(out, s),
        Value::Array(items) => {
            if items.is_empty() { out.push_str("[]"); return; }
            out.push('[');
            out.push('\n');
            for (i, item) in items.iter().enumerate() {
                emit_indent(out, depth + 1);
                write_value(out, item, depth + 1);
                if i + 1 < items.len() { out.push(','); }
                out.push('\n');
            }
            emit_indent(out, depth);
            out.push(']');
        }
        Value::Object(entries) => {
            if entries.is_empty() { out.push_str("{}"); return; }
            out.push('{');
            out.push('\n');
            for (i, (k, val)) in entries.iter().enumerate() {
                emit_indent(out, depth + 1);
                emit_string(out, k);
                out.push_str(": ");
                write_value(out, val, depth + 1);
                if i + 1 < entries.len() { out.push(','); }
                out.push('\n');
            }
            emit_indent(out, depth);
            out.push('}');
        }
    }
}

// ---------- App entry ----------

const INPUT_NAME: &[u8] = b"TEST.JSON\0";
const MAX_INPUT: usize = 8192;

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    let mut buf = Vec::with_capacity(MAX_INPUT);
    buf.resize(MAX_INPUT, 0u8);
    let n = unsafe {
        sys_fread(INPUT_NAME.as_ptr(), buf.as_mut_ptr(), MAX_INPUT as u32)
    };
    if n < 0 {
        eprintln!("json: FAIL fread rc={}", n);
        exit(1);
    }
    println!("json: read {} bytes", n);
    buf.truncate(n as usize);

    let text = match core::str::from_utf8(&buf) {
        Ok(t) => t,
        Err(_) => { eprintln!("json: FAIL non-utf8"); exit(1); }
    };
    let v = match parse(text) {
        Ok(v) => v,
        Err(e) => {
            eprintln!("json: FAIL parse {} at pos {}", e.msg, e.pos);
            exit(1);
        }
    };
    println!("json: parsed");

    let out = pretty(&v);
    println!("json: pretty start");
    for line in out.split('\n') {
        println!("{}", line);
    }
    println!("json: pretty end");
    println!("json: PASS");
    exit(0);
}
