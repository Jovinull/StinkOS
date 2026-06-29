//! StinkOS userland Rust shim. Wraps the libstink C syscalls into an
//! ergonomic surface for Rust apps: println!, eprintln!, read_line,
//! plus a default #[panic_handler] that funnels through sys_log +
//! sys_exit_code(1).
//!
//! Usage from an app crate:
//!
//! ```ignore
//! #![no_std]
//! #![no_main]
//! use libstink::{println, eprintln, read_line, exit};
//!
//! #[unsafe(no_mangle)]
//! pub extern "C" fn main() {
//!     println!("hello {}", 42);
//!     exit(0);
//! }
//! ```
//!
//! All formatting uses `core::fmt::Write` into a 256-byte stack
//! buffer; longer lines are truncated and tagged "...".

#![no_std]

use core::fmt::{self, Write};
use core::panic::PanicInfo;

extern "C" {
    fn sys_log(msg: *const u8);
    fn sys_exit_code(code: i32) -> !;
    fn sys_getkey() -> i32;
}

/// One sys_log call = one ring3 log line on serial. The kernel
/// prefixes "ring3: " and appends '\n', so callers pass a plain
/// NUL-terminated string with no leading prefix and no trailing '\n'.
pub fn log_raw(msg: &[u8]) {
    unsafe { sys_log(msg.as_ptr()); }
}

/// Exit immediately with the given code. Mirrors `process::exit(N)`.
pub fn exit(code: i32) -> ! {
    unsafe { sys_exit_code(code) }
}

const LINE_BUF: usize = 256;

struct LineBuf {
    buf: [u8; LINE_BUF],
    len: usize,
    /// Set when a write was truncated; we append "..." on flush so
    /// the user sees the truncation rather than silent loss.
    truncated: bool,
}

impl LineBuf {
    const fn new() -> Self {
        Self { buf: [0; LINE_BUF], len: 0, truncated: false }
    }
    fn reset(&mut self) {
        self.len = 0;
        self.truncated = false;
    }
    fn flush_as_log(&mut self) {
        // Tag truncations so callers can tell their format got cut.
        if self.truncated && self.len + 3 < LINE_BUF {
            self.buf[self.len    ] = b'.';
            self.buf[self.len + 1] = b'.';
            self.buf[self.len + 2] = b'.';
            self.len += 3;
        }
        // NUL terminate inside the buffer (we reserve LINE_BUF-1 above).
        let term = if self.len < LINE_BUF { self.len } else { LINE_BUF - 1 };
        self.buf[term] = 0;
        log_raw(&self.buf[..=term]);
        self.reset();
    }
}

impl Write for LineBuf {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        for &b in s.as_bytes() {
            if self.len >= LINE_BUF - 4 {
                // Reserve room for "..." + NUL in flush_as_log.
                self.truncated = true;
                break;
            }
            self.buf[self.len] = b;
            self.len += 1;
        }
        Ok(())
    }
}

/// Public: emit a formatted line via sys_log. The kernel handles the
/// trailing newline, so callers should NOT include '\n' in the format
/// string -- one println! = one line.
#[doc(hidden)]
pub fn _print(args: fmt::Arguments) {
    let mut buf = LineBuf::new();
    let _ = buf.write_fmt(args);
    buf.flush_as_log();
}

#[macro_export]
macro_rules! println {
    ($($arg:tt)*) => {{
        $crate::_print(::core::format_args!($($arg)*));
    }};
}

/// Same as println! today (sys_log is the only serial channel). When
/// a real stderr channel lands, this routes there; the macro stays
/// stable. Kept distinct so callers can tag their intent.
#[macro_export]
macro_rules! eprintln {
    ($($arg:tt)*) => {{
        $crate::_print(::core::format_args!($($arg)*));
    }};
}

/// Blocking key read; returns 0 if no key (caller should usually loop).
/// Mirrors sys_getkey directly; for line input use `read_line`.
pub fn getkey() -> i32 {
    unsafe { sys_getkey() }
}

/// Read a line of input into the caller's buffer, returning the count
/// of bytes consumed (not counting the terminating newline). Blocks
/// until Enter (key code 0x0A / 0x0D) or until the buffer fills.
pub fn read_line(buf: &mut [u8]) -> usize {
    let mut n = 0usize;
    while n < buf.len() {
        let k = getkey();
        if k == 0 { continue; }
        let c = (k & 0xFF) as u8;
        if c == b'\n' || c == b'\r' {
            return n;
        }
        buf[n] = c;
        n += 1;
    }
    n
}

/// Default panic handler routes through sys_log + exit(1). Apps that
/// want different behavior should NOT depend on libstink-as-crate and
/// supply their own #[panic_handler].
#[panic_handler]
fn on_panic(_info: &PanicInfo) -> ! {
    log_raw(b"libstink: rust panic, exiting\0");
    exit(1);
}
