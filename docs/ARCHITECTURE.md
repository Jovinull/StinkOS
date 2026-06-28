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
  arch/      paging, PMM, GDT, low-level I/O macros
  drivers/   audio, input, net, storage, video, misc (RTC, serial)
  fs/        StinkFS on-disk format, MBR helpers, VFS descriptor layer
  sys/       interrupts, syscalls, process table, pipes, ELF loader, boot diag
  ui/        graphical menu / launcher
lib/         libstink: userland C wrappers, allocator, stdio, http, sha256
apps/        ring-3 programs (shell, snake, doom, installer, stink-pkg, ...)
tools/       Python helpers (StinkFS image builder, WAD fetcher, repo server)
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
   serial → GDT → PMM → paging → VBE/FB → IDT → PIT → `proc_init` → keyboard
   → mouse → audio (SB16) → PCI → ATA (incl. PIIX Bus-Master DMA) → e1000
   → DHCP → `sti` → POST panel → `menu_run`.

If video is absent the kernel halts after the diag; if any non-critical device
is missing (SB16, e1000, ATA) the corresponding subsystem stays disabled and
the rest of the system runs unchanged.

## Address space

Single page directory, identity-mapped 4 GiB with `PG_PS` 4 MiB pages, plus
4 KiB page tables carved out for the userland window.

```
0x00000000 .. 0x003FFFFF   BIOS / kernel image (identity, supervisor)
0x00400000 .. 0x013FFFFF   user region (16 MiB, USER_PDES=4)
                           ├─ 0x00400000 .. 0x004FFFFF  code+data+bss
                           ├─ 0x00500000 .. 0x0053FFFF  user stack
                           └─ 0x00540000 .. 0x013FFFFF  user heap (sbrk/mmap)
0x10000000 .. 0x103FFFFF   USER_FB_BASE (mapped on demand via SYS_MAP_FB)
remaining 4 GiB            kernel identity map, supervisor-only
```

Heap pages are allocated lazily (`paging_user_alloc` bumps a pointer); mmap
allocations use the same arena, and the bump pointer never rewinds inside a
single app run. `paging_reset_user_heap` releases every user frame at app
exit.

## Process model

Multitasking is cooperative-preemptive: the round-robin scheduler runs at
the PIT IRQ tick (100 Hz) so any thread that doesn't disable interrupts gets
preempted. The data plane lives in `kernel/sys/proc.{c,h}`.

* **PCB** (`struct proc`): PID, parent PID, state, exit code, kernel stack
  top + saved ESP, optional CR3 (currently 0 = share the kernel page dir),
  pending-signal bitmap, signal-handler table, per-process VFS descriptor
  table, name.
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

## Subsystem highlights

* **Audio** — SB16 driver, ISA DMA channel 1, half-buffer IRQs at 22050 Hz
  mono u8. The kernel mixer (8 channels, fixed-point volume, saturating)
  fills whichever buffer half just finished playing.
* **Network** — PCI mechanism 1 → e1000 NIC → ethernet → ARP / IPv4 / ICMP /
  UDP / DHCP / DNS / TCP. TCP has a retransmit timer with exponential
  backoff; LISTEN sockets accept one connection in-place. See
  [NETWORK.md](NETWORK.md).
* **Filesystem** — StinkFS: a flat fixed-position 2-sector directory at
  LBA 128 followed by ~100 MiB of data. The VFS layer (`vfs.c`) exposes
  POSIX-ish descriptors over it, with a per-process 16-slot table embedded
  in the PCB. See [STINKFS.md](STINKFS.md).
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

The current launcher model is "one ring-3 app at a time" because user-space
still shares one address space. The scheduler is wired in but only juggles
kernel threads today; per-process CR3s + a true `fork`/`exec` arrive in a
later commit.

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
