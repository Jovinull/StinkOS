<div align="center"><pre>
███████╗████████╗██╗███╗   ██╗██╗  ██╗ ██████╗ ███████╗
██╔════╝╚══██╔══╝██║████╗  ██║██║ ██╔╝██╔═══██╗██╔════╝
███████╗   ██║   ██║██╔██╗ ██║█████╔╝ ██║   ██║███████╗
╚════██║   ██║   ██║██║╚██╗██║██╔═██╗ ██║   ██║╚════██║
███████║   ██║   ██║██║ ╚████║██║  ██╗╚██████╔╝███████║
╚══════╝   ╚═╝   ╚═╝╚═╝  ╚═══╝╚═╝  ╚═╝ ╚═════╝ ╚══════╝
       An x86 PC operating system. Written from scratch.
                  Boots real hardware. No libc. No regrets.
</pre></div>

<p align="center"><strong>boot sector → kernel → paging → ring 3 → syscalls → filesystem · zero external dependencies · C + assembly</strong></p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPLv3-blue.svg" alt="License: GPLv3"></a>
  <img src="https://img.shields.io/badge/arch-x86%20i386-orange.svg" alt="x86 i386">
  <img src="https://img.shields.io/badge/lang-C%20%2B%20Assembly-red.svg" alt="C + Assembly">
  <img src="https://img.shields.io/badge/runs%20on-QEMU%20%2B%20real%20PC-green.svg" alt="Runs on QEMU and real PC">
  <img src="https://img.shields.io/badge/PRs-welcome-brightgreen.svg" alt="PRs welcome">
</p>

<p align="center">
  <a href="#what-this-actually-is">What it is</a> ·
  <a href="#what-it-does-today">What it does</a> ·
  <a href="#run-it-2-minutes">Run it</a> ·
  <a href="#syscalls">Syscalls</a> ·
  <a href="#write-an-app">Write an app</a> ·
  <a href="#roadmap">Roadmap</a> ·
  <a href="CONTRIBUTING.md">Contribute</a>
</p>

---

## What this actually is

A real, 32-bit x86 operating system. Not a Linux distro with a custom theme. Not a tutorial that stops at "Hello, kernel!"

StinkOS boots a PC (or QEMU). It enters protected mode, sets up paging, launches user programs in Ring 3 with their own isolated address spaces, and lets them call back into the kernel through a syscall ABI. Same shape as a real OS — just smaller in every direction.

Everything in this repo was written from a blank disk image. The bootloader. The kernel. The drivers. The filesystem. The userland C library. The apps. No libc. No prebuilt kernel. No imported bootloader. Just `boot.s`, a kernel in C, a handful of drivers, and a flat-file filesystem I cobbled together.

Yeah, the name is a joke. The code isn't.

## Screenshots

<p align="center">
  <img src="images/menu.png" width="48%" alt="StinkOS Main Menu with RTC and Mouse">
  <img src="images/shell.png" width="48%" alt="StinkOS Graphical Shell">
</p>
<p align="center">
  <img src="images/pong.png" width="48%" alt="Pong Game (AI vs Player)">
  <img src="images/snake.png" width="48%" alt="Snake Game">
</p>

## What it does today

On boot, StinkOS will:

1. **Load itself from disk** via the BIOS, enable A20, build a GDT, and switch the CPU into 32-bit protected mode. (`boot.s`)
2. **Set a VBE linear framebuffer** at 1024×768×32 — pixels go straight to video memory. (`vbe.c`, `fb.c`)
3. **Wire up interrupts** — IDT, remap the PIC, configure the PIT at 100 Hz, drive the PS/2 keyboard. (`interrupts.c`, `keyboard.c`)
4. **Enable paging** with a 4 MiB identity map for the kernel, backed by a physical frame allocator. Userland gets its own isolated 4 KiB-paged region. (`paging.c`, `pmm.c`)
5. **Install a TSS** and drop into a graphical start menu. (`menu.c`, `gdt.c`)
6. **Load and run apps in Ring 3.** The kernel reads an ELF binary from a raw disk slot, copies it into a fresh user address space, and `iret`s into user code. The app talks to the kernel only through `int 0x80`. When it exits or faults, control returns to the menu — a misbehaving app cannot take down the system. (`elf.c`, `usermode_asm.s`)

There's also a filesystem of my own design (`fs.c` — "StinkFS"), a PC speaker driver (`speaker.c`), an ATA disk driver (`ata.c`), and a serial console used for debug output (visible in the QEMU stdio log).

## Run it (2 minutes)

You need an `i386-elf` cross-compiler and `qemu-system-i386`.

```bash
# one-time: build the cross-toolchain (~30-60 min)
bash tools/build-cross-toolchain.sh
source ~/.bashrc

# build and boot in QEMU
make run
```

Want to verify it boots without staring at it?

```bash
make test-headless     # boots in qemu, asserts via serial log
```

Other useful targets: `make` (build only), `make hex` (hexdump the disk image), `make dall` (disassemble), `make clean`.

## Syscalls

Userland talks to the kernel via `int 0x80`. `eax` = syscall number, args in `ebx`/`ecx`/`edx`, return value in `eax`.

| # | Name | Arguments | Effect |
|--:|------|-----------|--------|
| 1 | `log`     | `ebx`=str                        | Write a line to the debug serial console |
| 2 | `draw`    | `ebx`=x `ecx`=y `edx`=rgb         | Plot a pixel |
| 3 | `getkey`  | —                                 | Return next keypress (0 if none) |
| 4 | `alloc`   | —                                 | Hand back a fresh page of user memory |
| 5 | `exit`    | —                                 | Return to the menu |
| 6 | `ticks`   | —                                 | Timer ticks since boot (10 ms each) |
| 7 | `sound`   | `ebx`=hz                          | Beep at frequency (0 = silence) |
| 8 | `fwrite`  | `ebx`=name `ecx`=buf `edx`=size   | Write a file in StinkFS |
| 9 | `fread`   | `ebx`=name `ecx`=buf `edx`=max    | Read a file from StinkFS |
| 10 | `fcount` | —                                 | Number of files in StinkFS |
| 11 | `finfo`  | `ebx`=idx `ecx`=name_buf          | Name of the i-th file |
| 12 | `fdelete`| `ebx`=name                        | Delete a file |
| 13 | `fappend`| `ebx`=name `ecx`=buf `edx`=size   | Append to an existing file |

C apps don't write `int 0x80` by hand — `apps/libstink.h` wraps each call as a static inline. Drop it in and call `sys_draw(x, y, 0xff00ff)`.

## Userland apps

Apps are independent ELF binaries. They live on raw disk slots (LBAs 64, 72, 80, …) and show up in the start menu at boot:

| Name | What it is |
|------|------------|
| `HELLO` / `BOX`       | Assembly demos — log to serial, paint pixels |
| `FAULT`               | Deliberately touches kernel memory, gets killed cleanly |
| `GAME`                | Keyboard-controlled block (assembly) |
| `HIC` / `ANIM`        | First C apps — prove `crt0.s` + `libstink.h` work |
| `BEEP`                | Plays notes through the PC speaker |
| `SAVE` / `FILES` / `LS` / `DEL` | StinkFS demos: write, list, read, delete |
| `PLAY`                | A tiny collector game that persists its high score in StinkFS |

## Write an app

```c
#include "libstink.h"

void main(void) {
    sys_log("hello from ring 3");

    for (int y = 100; y < 300; y++)
        for (int x = 100; x < 500; x++)
            sys_draw(x, y, 0x00ffaa);

    while (sys_getkey() != 27) { /* wait for ESC */ }
    sys_exit();
}
```

Add a build rule beside the others in the `Makefile`, pick a free LBA slot, wire it into the TOC line at the end of the rule, and `make run`. The menu picks it up automatically.

## Components

| Area | Files |
|------|-------|
| Boot, protected-mode entry           | `boot.s`, `linker.ld` |
| Kernel entry                         | `kernel.c` |
| Interrupts (IDT, PIC, PIT, syscalls) | `interrupts.c`, `interrupts_asm.s` |
| GDT + TSS                            | `gdt.c`, `gdt_asm.s` |
| Paging, physical memory              | `paging.c`, `pmm.c` |
| Video (VBE + framebuffer + font)     | `vbe.c`, `fb.c`, `font.c` |
| Drivers (serial, keyboard, ATA, speaker) | `serial.c`, `keyboard.c`, `ata.c`, `speaker.c` |
| StinkFS (named flat files)           | `fs.c`, `fs.h` |
| ELF loader + Ring 3 entry            | `elf.c`, `menu.c`, `usermode_asm.s` |
| Userland C library                   | `apps/libstink.h`, `apps/crt0.s` |

## Toolchain — why a cross-compiler?

The host `gcc` assumes it's producing programs for the host OS. Forcing it to emit bare-metal code works for tiny examples and becomes a source of subtle bugs as the OS grows. The `i386-elf` cross-compiler carries none of those assumptions — it produces ELF binaries for a freestanding i386 target, period. It's the boring, correct foundation.

`tools/build-cross-toolchain.sh` builds binutils + gcc for the `i386-elf` target into `~/opt/cross`. Once, then forget it exists.

## Roadmap

**Done:**

- [x] 32-bit protected mode (LBA load, A20, GDT, TSS)
- [x] VBE linear framebuffer, font + text rendering
- [x] Interrupts: IDT, PIC remap, PIT timer, PS/2 keyboard
- [x] Paging + physical frame allocator
- [x] Userland (Ring 3), isolated address space, fault isolation
- [x] ELF program loader
- [x] StinkFS (flat named files, persisted to disk)
- [x] Sound (PC speaker)
- [x] Start menu

**Up next** (in no particular order):

- [ ] Userland heap (`sys_brk` / a real allocator)
- [ ] Arrow keys in the PS/2 driver
- [ ] Larger app slots (or a proper on-disk app layout)
- [ ] Framebuffer-mapping syscall so apps can blit faster than per-pixel
- [ ] `printf` and string primitives in `libstink.h`
- [ ] More games. Maybe a shell. Maybe a port of something old.

## Contributing

PRs welcome. Start with [CONTRIBUTING.md](CONTRIBUTING.md) — it's short. For anything bigger than a bug fix, open an issue first so we can agree on shape before you spend a weekend on it.

By participating you agree to the [Code of Conduct](CODE_OF_CONDUCT.md). Found a security issue? See [SECURITY.md](SECURITY.md).

## Acknowledgments

StinkOS exists because [@suwateru](https://github.com/suwateru) got me into low-level back in mid-2024 — my first C, my first segfaults, my first time the stack pointer actually made sense. The project is based on her original [StinkOS](https://github.com/suwateru/StinkOS); what's here now is two years of self-study built on what she started.

## License

[GPLv3](LICENSE). Use it, fork it, learn from it, run it on weird hardware, write a thesis about it — but if you distribute something derived from this code, the source has to stay open. That's the deal.

## Why "Stink"?

Honestly? Because the first version was held together with duct tape and prayers, and the name kind of stuck. By the time it was good enough to not stink, the joke was funnier than any "serious" name I could come up with. Embrace the bit.
