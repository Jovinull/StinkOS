//! Validates that the `alloc` crate works under our libstink
//! `malloc`/`free` backing via a #[global_allocator] shim. Exercises
//! Box<T> (single alloc + drop), Vec<T> (multiple alloc + growth +
//! drop), and a manual write-back path that proves freed memory
//! actually returns to the K&R first-fit free list.
//!
//! Output keys for the smoke driver:
//!   "rs-alloc: start"
//!   "rs-alloc: box ok"
//!   "rs-alloc: vec ok sum=N"
//!   "rs-alloc: free reuse ok"
//!   "rs-alloc: PASS"

#![no_std]
#![no_main]

extern crate alloc;

use alloc::boxed::Box;
use alloc::vec::Vec;
use core::alloc::{GlobalAlloc, Layout};
use core::panic::PanicInfo;

extern "C" {
    fn sys_log(msg: *const u8);
    fn sys_exit() -> !;
    fn malloc(n: usize) -> *mut u8;
    fn free(p: *mut u8);
}

struct LibStinkAllocator;

unsafe impl GlobalAlloc for LibStinkAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        // libstink's K&R allocator returns 8-byte-aligned blocks
        // (the header union pads with a `double`), enough for u64 /
        // most layouts. Anything that demands more alignment than 8
        // gets a NULL back, which surfaces as a Rust OOM panic.
        if layout.align() > 8 {
            return core::ptr::null_mut();
        }
        unsafe { malloc(layout.size()) }
    }
    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        unsafe { free(ptr) }
    }
}

#[global_allocator]
static ALLOC: LibStinkAllocator = LibStinkAllocator;

fn log(msg: &[u8]) {
    // Caller must NUL-terminate inside `msg`; this helper just trusts.
    unsafe { sys_log(msg.as_ptr()); }
}

/// Tiny u32 -> hex/dec stringifier so we can log Vec sums without
/// pulling in core::fmt::write (which would need a sized format
/// buffer). Writes at most 11 ASCII digits + NUL into the given buf.
fn write_u32(buf: &mut [u8; 16], v: u32) -> usize {
    let mut n = v;
    let mut digits = [0u8; 11];
    let mut i = 0usize;
    if n == 0 {
        digits[0] = b'0';
        i = 1;
    } else {
        while n > 0 {
            digits[i] = b'0' + (n % 10) as u8;
            n /= 10;
            i += 1;
        }
    }
    // reverse
    for j in 0..i {
        buf[j] = digits[i - 1 - j];
    }
    buf[i] = 0;
    i
}

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    log(b"rs-alloc: start\0");

    // 1. Single Box<T>
    let b: Box<u32> = Box::new(0xDEADBEEFu32);
    if *b == 0xDEADBEEF {
        log(b"rs-alloc: box ok\0");
    } else {
        log(b"rs-alloc: FAIL box\0");
        unsafe { sys_exit() }
    }
    drop(b);

    // 2. Vec<u32> with growth (forces realloc paths)
    let mut v: Vec<u32> = Vec::new();
    for i in 0..32u32 {
        v.push(i);
    }
    let sum: u32 = v.iter().copied().sum();
    if sum == 31 * 32 / 2 {
        let mut buf = [0u8; 16];
        let prefix = b"rs-alloc: vec ok sum=";
        let mut out = [0u8; 64];
        for (idx, &b) in prefix.iter().enumerate() {
            out[idx] = b;
        }
        let plen = prefix.len();
        let n = write_u32(&mut buf, sum);
        for i in 0..n {
            out[plen + i] = buf[i];
        }
        out[plen + n] = 0;
        log(&out as &[u8]);
    } else {
        log(b"rs-alloc: FAIL vec sum\0");
        unsafe { sys_exit() }
    }
    drop(v);

    // 3. Free + re-alloc loop: a leaking allocator would push the
    //    backing brk out by ~256 KiB after 1000 round-trips and the
    //    later iterations would OOM well before the loop finishes.
    //    A working free() bounds the working set; the loop completes
    //    fully and the last alloc still returns a valid pointer.
    let mut last_ok: *const u8 = core::ptr::null();
    for _ in 0..1000 {
        let b: Box<[u8; 256]> = Box::new([0x55u8; 256]);
        last_ok = b.as_ptr() as *const u8;
        drop(b);
    }
    if !last_ok.is_null() {
        log(b"rs-alloc: free reuse ok\0");
    } else {
        log(b"rs-alloc: FAIL alloc loop OOM\0");
        unsafe { sys_exit() }
    }

    log(b"rs-alloc: PASS\0");
}

#[panic_handler]
fn on_panic(_info: &PanicInfo) -> ! {
    unsafe {
        sys_log(b"rs-alloc: rust panic\0".as_ptr());
        sys_exit();
    }
}
