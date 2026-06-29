//! Exercises the libstink Rust shim: println! with format args of
//! several types, eprintln!, exit codes. The smoke driver scrapes
//! the expected lines off serial to confirm core::fmt works through
//! our LineBuf + sys_log path.

#![no_std]
#![no_main]

extern crate alloc;
use core::alloc::{GlobalAlloc, Layout};
use libstink::{println, eprintln, exit};

extern "C" {
    fn malloc(n: usize) -> *mut u8;
    fn free(p: *mut u8);
}

struct LibStinkAllocator;

unsafe impl GlobalAlloc for LibStinkAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
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

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    println!("rs-stdio: start");
    println!("rs-stdio: integer={}", 42);
    println!("rs-stdio: hex=0x{:X}", 0xDEADu32);
    println!("rs-stdio: multi a={} b={} c={}", 1u32, "two", 3.5);
    // alloc-formatted string round trip
    let s = alloc::format!("alloc-string-{}", 7);
    println!("rs-stdio: built={}", s);
    eprintln!("rs-stdio: eprintln-channel");
    println!("rs-stdio: PASS");
    exit(0);
}
