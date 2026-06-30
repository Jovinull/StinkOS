# StinkOS Architecture

A high-level tour of the kernel: how it boots, how it lays out memory, how it
schedules work, and how a ring-3 app talks to it. Subsystem deep-dives live in
their own documents (see [SYSCALLS.md](SYSCALLS.md), [NETWORK.md](NETWORK.md),
[STINKFS.md](STINKFS.md), [PACKAGING.md](PACKAGING.md)). The on-disk source of
truth is the code itself — this document only summarises the shape.

## Repository layout

```
boot/        16-bit + 32-bit boot stages, GDT/IDT/usermode asm stubs
kernel/
  arch/      PAE paging, PMM (refcount-aware), GDT, ACPI, CPUID, I/O macros
  drivers/   audio, input, net, storage, video, misc (RTC, serial)
  fs/        StinkFS on-disk format, MBR helpers, VFS multi-mount + ops table
  sys/       interrupts, syscalls, process table, fork/exec/wait, COW handler,
             pipes, signals, ELF loader, boot diag
  ui/        graphical menu / launcher
lib/         libstink: userland C wrappers (static inline) + libstink_syms.c
             (non-inline shims for Rust extern "C"), allocator, stdio, http, sha256
apps/        ring-3 programs (shell, snake, doom, installer, stink-pkg, ...)
  rust/      Rust userland: i686-stinkos target spec, shared libstink rlib,
             rs-hello / rs-alloc / rs-stdio / rs-json / rs-life
tools/       Python helpers (StinkFS image builder, WAD fetcher, repo server,
             smoke drivers, screenshot capture)
```

`kernel/defs.h` is an umbrella header that re-includes every subsystem
interface, so most kernel translation units write a single `#include "defs.h"`.

## Boot path

The whole boot is one straight line, no fallbacks beyond timing-out missing
hardware:

1. **16-bit boot sector** (`boot/boot.s`): loaded by BIOS at `0x7C00`.
   Sets up VBE, enables A20, reads the rest of the bootblock
   (`BOOTBLOCK_SECTORS = 16`, i.e. ~8 KiB) into `0x7E00..`, loads a flat
   GDT, and far-jumps into 32-bit mode.
2. **`pm_entry`** (tail of `boot/boot.s`): zeroes the bootblock's `.bss`,
   sets up segment registers + protected-mode stack (`STACK_TOP = 0x90000`),
   and `call`s `bootmain` (in `boot/bootmain.c`).
3. **`bootmain`** reads the kernel ELF off disk starting at
   `KERNEL_LBA = 16` via ATA PIO, walks the PT_LOAD program headers,
   copies each segment to its linked physical address (kernel is linked
   at `0x100000` by `boot/kernel.ld`), zero-fills any `memsz > filesz`
   tail (the kernel `.bss`), and jumps to the ELF entry. From that
   moment, the bootblock memory (`0x7C00..0x9FFF`) is dead.
4. **`_kernel_start`** (`kernel/arch/kentry.s`) resets the segment
   registers + stack, zeroes the kernel `.bss`, and calls `kmain`.
5. **`kmain`** (`kernel/main.c`) wires every subsystem in a fixed order,
   each followed by a one-line `bootdiag_add()` for the POST screen:
   serial → CPUID (PAE + NX detect) → GDT → PMM (refcount table) → paging
   (PAE 3-level, W^X for kernel image) → VBE/FB → IDT → PIT → `proc_init`
   → ACPI (RSDP scan, RSDT/XSDT walk, FADT + MADT parse) → keyboard
   → mouse → audio (SB16) → PCI → ATA (incl. PIIX Bus-Master DMA) → e1000
   → DHCP → VFS multi-mount (`A:` slot 0 auto-registered) → `sti`
   → POST panel → `menu_run`.

If video is absent the kernel halts after the diag; if any non-critical device
is missing (SB16, e1000, ATA) the corresponding subsystem stays disabled and
the rest of the system runs unchanged.

## Address space

Higher-half kernel (xv6-public pattern). Kernel image is **linked** at
virt `0x80100000` and **loaded** at phys `0x100000` via per-section
`AT(ADDR - 0x80000000)` in `boot/kernel.ld`. Bootmain runs with paging
off, copies each PT_LOAD to `p_paddr`, and calls `e_entry` (resolved by
`kentry.s` to the *physical* address of the asm entry trampoline). The
trampoline installs a bootstrap pgdir (identity-low + high-half mirror),
enables paging, then far-jumps into the high-half C kernel.

Each process owns a private page directory. Kernel PDEs are identical
across every pgdir; only the user window differs.

```
virt                                       phys
0x00000000 .. 0x013FFFFF   USER region    per-process 4 KiB pages
                           ├─ 0x00400000  code+data+bss   (USER_PDES=4)
                           ├─ 0x00500000  user stack
                           └─ 0x00540000  user heap (sbrk/mmap)
0x10000000 .. 0x103FFFFF   USER_FB_BASE   physical LFB (SYS_MAP_FB)
0x80000000 .. 0x8FFFFFFF   KERNEL direct  phys [0, 256MB) via 4 MiB PSE
0xFD000000 .. 0xFFFFFFFF   DEVSPACE       MMIO identity (LFB, e1000 BAR)
```

Kernel reads/writes every physical frame as `*KVA(phys)` where
`KVA(p) = (T*)(p + KERNBASE)`. The high-half mirror sits above
`KERNBASE = 0x80000000`, which is **above the highest user virt**
(`USER_END = 0x1400000`) — so no user PT can ever shadow a kernel
deref. This eliminates the entire class of "kernel writes phys frame
N, frame N happens to be aliased by some proc's user PT" bugs that bit
us during the §1 fork landing (see commit `5b00964`).

Heap pages are allocated lazily (`paging_user_alloc` bumps a pointer);
mmap allocations use the same arena, and the bump pointer never rewinds
inside a single app run. `paging_reset_user_heap` releases every user
frame at app exit.

## Process model

Multitasking is cooperative-preemptive: the round-robin scheduler runs at
the PIT IRQ tick (100 Hz) so any thread that doesn't disable interrupts gets
preempted. The data plane lives in `kernel/sys/proc.{c,h}`.

* **PCB** (`struct proc`): PID, parent PID, state, exit code, kernel stack
  top + saved ESP, CR3 (KVA pointer to per-process pgdir; `paging_switch`
  applies `V2P` before the actual CR3 load), pending-signal bitmap,
  signal-handler table, per-process VFS descriptor table, name.
* **Process table**: fixed `PROC_MAX = 16` slots. PID = slot index + 1; PID 1
  (`kinit`) is the boot process and is non-killable.
* **States**: UNUSED → EMBRYO → READY ↔ RUNNING → ZOMBIE (reaped by parent).
* **Context switch**: `boot/context_asm.s` saves the SysV callee-saved regs
  (EBX/ESI/EDI/EBP) + EFLAGS, swaps ESP, restores them. New stacks are
  pre-built by `context_init` so the first switch-in `ret`s into the entry
  function with a clean EFLAGS.
* **Scheduler** (`proc_yield`): round-robins from current PID forward,
  swapping in the next PROC_READY slot. No-op when nothing else is ready,
  so single-process kernels stay bit-identical to the pre-scheduler boot.

### Concurrency primitives

* **Pipes** (`kernel/sys/pipe.c`): 8 anonymous pipes, each 4 KiB ring buffer
  with one read and one write endpoint, blocking semantics, EPIPE/EOF on
  endpoint closure.
* **Signals**: cooperative — `sys_raise(pid, sig)` sets a bit in the target's
  PCB; `sys_sigpoll()` drains one bit and returns its number. The app calls
  its own registered handler. Avoids the iret-frame surgery of true async
  signals.
* **Wait/waitpid**: `sys_wait` blocks on `sti+hlt+cli` until any child
  becomes ZOMBIE, then reaps (frees kernel stack + PCB) and returns the
  exit code. `sys_waitpid` is the same for a specific PID.

## Syscalls

Single gate at vector `0x80`, DPL=3. ABI: `eax` = number, `ebx/ecx/edx/esi`
= args, result in `eax`. The dispatcher (`kernel/sys/syscall.c`) is one big
switch; argument pointers are validated via `paging_user_range_ok` before
any read or write. See [SYSCALLS.md](SYSCALLS.md) for the full numbered
table.

## Rust userland

A Rust userland sits next to the C apps. Toolchain is `cargo` + nightly
+ `rust-src` + a custom `i686-stinkos.json` target spec (i686 bare-metal,
panic=abort, no SSE/MMX). Crates are built with `-Z build-std=core,alloc`
so `core` and `alloc` compile from source against the target, and link
as a `staticlib` (`.a`). The linker pulls that `.a` with `--whole-archive`
alongside the existing C `apps/crt0.s` -- one ELF, one entry point, no
runtime split.

Two libstinks coexist:

* **`lib/libstink.h`** is `static inline` everywhere -- C apps consume
  it header-only, zero symbols emitted.
* **`lib/libstink_syms.c`** re-exports the same surface as **linkable
  symbols** so Rust `extern "C"` declarations resolve at link time
  (sys_log / sys_exit / sys_exit_code / sys_draw / sys_getkey /
  sys_alloc / sys_ticks / sys_sound / sys_fwrite / sys_fread + the
  freestanding `memcpy` / `memset` / `memmove` / `memcmp` that Rust's
  `core` calls into).
* **`apps/rust/libstink/`** is a Rust rlib that wraps the C surface in
  ergonomic helpers: `println!` / `eprintln!` macros over `core::fmt::Write`,
  `read_line(buf)`, `exit(code)`, a default `#[panic_handler]` that
  logs + exits 1, and a `#[global_allocator]` shim that routes Rust's
  `alloc` crate (Box/Vec/String/...) through the libstink K&R `malloc`.

Apps today:

| Name        | Purpose |
|-------------|---------|
| `rs-hello`  | First Rust ELF; `extern "C" fn main` + `sys_log` -- toolchain proof |
| `rs-alloc`  | `Box::new`, `Vec` growth, free + realloc loop through `#[global_allocator]` |
| `rs-stdio`  | `println!` / `eprintln!` / multi-arg format / `alloc::format!` |
| `rs-json`   | ~390 LOC recursive-descent JSON parser, zero external crates; reads `TEST.JSON` from StinkFS and pretty-prints |
| `rs-life`   | Conway's Game of Life on the framebuffer; Gosper glider gun seed, 128x96 toroidal grid, 8x8 px cells, diff-painting |

Each app ships a dedicated smoke (`tools/smoke-rs-*.py`) that drives
QEMU + asserts a feature-specific invariant. CI runs all five.

## Subsystem highlights

* **Audio** — SB16 driver, ISA DMA channel 1, half-buffer IRQs at 22050 Hz
  mono u8. The kernel mixer (8 channels, fixed-point volume, saturating)
  fills whichever buffer half just finished playing.
* **Network** — PCI mechanism 1 → e1000 NIC → ethernet → ARP / IPv4 / ICMP /
  UDP / DHCP / DNS / TCP. TCP has a retransmit timer with exponential
  backoff; LISTEN sockets accept one connection in-place. See
  [NETWORK.md](NETWORK.md).
* **Filesystem** — StinkFS: a flat fixed-position **4-sector** directory at
  LBA 128 (80 file slots) followed by ~100 MiB of data. Routed through a
  2-slot `fs_mount` table behind an `fs_ops` dispatch (v0.8); filenames
  may carry an optional DOS-style 2-char prefix (`A:foo` / `B:foo`) that
  picks the mount slot. `SYS_MOUNT` registers an additional StinkFS region
  at runtime (second disk under `B:`). The VFS layer (`vfs.c`) exposes
  POSIX-ish descriptors over the dispatched ops, with a per-process
  16-slot table embedded in the PCB. See [STINKFS.md](STINKFS.md).
* **ACPI** — RSDP scan (EBDA + BIOS area, 16-byte aligned, checksum-validated),
  RSDT/XSDT walker, FADT (S5 soft-off via PM1a_CNT_BLK port write) and
  MADT (Local APIC + IOAPIC topology). No AML interpreter; SLP_TYPa is
  hardcoded to 5 (the value used by essentially every PC firmware
  including QEMU). `sys_shutdown` tries ACPI first, falls back to legacy
  QEMU / Bochs / VirtualBox port-magic paths.
* **UI** — Linear framebuffer + bitmap font; the menu (`menu.c`) launches
  apps by ELF name, and the shell is just another app.

## App lifecycle

1. The shell or menu calls `SYS_EXEC(name)`.
2. The kernel resolves `NAME.ELF` in StinkFS, silences the mixer, frees the
   previous user heap, loads the ELF via `elf_load`, and `iret`s to ring 3
   with EIP at the program entry, ESP at `paging_user_stack_top`.
3. The app runs at CPL 3 with `IF` set, hitting `int 0x80` for kernel
   services.
4. On `SYS_EXIT` (or an unhandled fault) the kernel calls `app_return`,
   which re-launches the shell (if the app was `SYS_EXEC`-spawned) or the
   graphical menu (otherwise).

Per-process page directories and `sys_fork` / `sys_exec` / `sys_wait` /
`sys_waitpid` have landed (v0.4). The scheduler juggles multiple ring-3
processes simultaneously; the shell `bg <name>` builtin proves the path
end-to-end (fork + exec + parent stays at the prompt; `ps` lists both
procs). `sys_fork` is **copy-on-write** since v0.7 -- writable pages are
shared at fork time with `PG_COW` set (PTE software bit 9) and RW
cleared on both sides. The first writer takes a #PF; `paging_handle_cow_fault`
either lifts RW (refcount == 1, last owner) or allocates a fresh frame
+ memcpys + installs RW at the writer's PTE while decrementing the
shared frame's refcount. Read-only pages (`.text` / `.rodata` under
v0.5 W^X) stay shared without `PG_COW`; writes to those remain real
W^X violations and the trap path kills the offender as before.

The launcher model is "one menu-launched app at a time" but multiple
shell-backgrounded apps can run concurrently. Each owns a private
pgdir; CR3 swaps in the scheduler (`paging_switch` before every
`context_switch`).

## Build

Cross-toolchain target `i386-elf`. The repo's `tools/build-cross-toolchain.sh`
builds binutils + gcc against newlib-free freestanding C. `make` produces
`os.bin` (boot sector + kernel + StinkFS data region) ready for QEMU
(`make run`) or to be written byte-for-byte onto a fresh disk.

The build pipeline is intentionally simple: every C source under
`kernel/` compiles with the same `-O0 -m32 -ffreestanding` flags; every
userland app links `apps/crt0.s` + its `.c` + the libstink objects; the
StinkFS image is assembled by `tools/make-stinkfs.py` from the per-app ELFs
plus the bundled assets (Freedoom WADs, etc.).
