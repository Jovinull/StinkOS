//! First Rust app for StinkOS. Built as a `staticlib` (.a) and linked
//! with the existing C `crt0.s` (which calls `main`). The `main` symbol
//! is exported `extern "C"` so the C runtime hands control off to Rust.
//!
//! The single panic_handler routes through `sys_log` + `sys_exit` so a
//! Rust panic looks identical to a deliberate `sys_log("panic: ...")`
//! followed by termination on the kernel side.

#![no_std]
#![no_main]

use core::panic::PanicInfo;

extern "C" {
    fn sys_log(msg: *const u8);
    fn sys_exit() -> !;
}

#[unsafe(no_mangle)]
pub extern "C" fn main() {
    // SAFETY: literal has a trailing NUL byte; sys_log scans until NUL.
    unsafe { sys_log(b"rs-hello: hi from rust\0".as_ptr()); }
}

#[panic_handler]
fn on_panic(_info: &PanicInfo) -> ! {
    // SAFETY: literal NUL-terminated; sys_log scans to first NUL.
    unsafe {
        sys_log(b"rs-hello: rust panic, exiting\0".as_ptr());
        sys_exit();
    }
}
