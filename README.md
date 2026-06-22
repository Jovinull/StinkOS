# StinkOS

A real, standard x86 PC operating system, written from scratch. StinkOS runs in
**32-bit protected mode** with a proper split between **kernel space** and an
isolated **userland (Ring 3)**, a **syscall** contract, and a graphical start
menu that launches apps and games.

Games, a browser, and other apps are built inside this project but run as
**independent userland binaries** — not compiled into the kernel. The kernel
stays lean: it loads those binaries from disk and runs them in Ring 3, each in
its own isolated address space, through a loader.

The project is built with no external dependencies: bootloader, kernel, drivers,
and libraries are all written inside the repository.

## What it does today

On boot, StinkOS:

1. Loads the kernel from disk (LBA), enables A20, builds a GDT and switches the
   CPU to 32-bit protected mode.
2. Queries and sets a **VBE linear-framebuffer** video mode (real mode), then
   draws graphics directly to the framebuffer.
3. Sets up the **IDT**, remaps the **PIC**, and handles the **PIT timer** and
   **PS/2 keyboard** interrupts.
4. Enables **paging** (4 MiB identity map for the kernel) over a physical frame
   allocator, and gives userland its own **4 KiB-paged, isolated** region.
5. Installs a **TSS** and drops into a graphical **start menu**.
6. Lets you pick an app; the kernel loads it from a raw disk slot and runs it in
   **Ring 3**. The app talks to the kernel only through **syscalls**. When it
   exits (or faults), control returns to the menu — a misbehaving app cannot
   take down the system.

## Syscall contract (`int 0x80`)

`eax` = number, arguments in `ebx`/`ecx`/`edx`, result in `eax`.

| # | Name      | Arguments            | Effect |
|---|-----------|----------------------|--------|
| 1 | log       | `ebx`=string         | Write a line to the debug console |
| 2 | draw      | `ebx`=x `ecx`=y `edx`=rgb | Plot a pixel |
| 3 | getkey    | —                    | Next key (or 0 if none) |
| 4 | alloc     | —                    | A page of user memory |
| 5 | exit      | —                    | Return to the menu |
| 6 | ticks     | —                    | Timer ticks since boot |

## Components

| Area | Files |
|------|-------|
| Boot + protected mode | `boot.s`, `linker.ld` |
| Kernel entry | `kernel.c` |
| Interrupts (IDT/PIC/PIT, syscalls) | `interrupts.c`, `interrupts_asm.s` |
| GDT + TSS | `gdt.c`, `gdt_asm.s` |
| Paging + physical memory | `paging.c`, `pmm.c` |
| Video (VBE + framebuffer + font) | `vbe.c`, `fb.c`, `font.c` |
| Drivers (serial, keyboard, ATA, port I/O) | `serial.c`, `keyboard.c`, `ata.c`, `io.h` |
| Start menu + ring-3 entry | `menu.c`, `usermode_asm.s` |
| Userland apps | `apps/` (asm and C; `crt0.s` + `libstink.h` for C) |

## Userland apps

Apps live on raw disk slots and are listed in the menu. Current demos:

- **HELLO** / **BOX** — draw and use syscalls (assembly).
- **FAULT** — deliberately touches kernel memory to show it gets killed cleanly.
- **GAME** — a keyboard-controlled block.
- **HIC** — a C app (built with `crt0.s` + `libstink.h`).
- **ANIM** — a C app animating over time via the `ticks` syscall.

## Architecture and tooling

- Target: **x86** (i386), 32-bit protected mode (boots through real mode).
- Toolchain: a dedicated **`i386-elf` cross-compiler** (`i386-elf-as`,
  `i386-elf-gcc`, `i386-elf-ld`) — a compiler built for bare metal, not the host
  `gcc`.
- Emulator: `qemu-system-i386`.

> Why a cross-compiler and not the system `gcc`? The host `gcc` assumes it is
> producing programs for Linux. Forcing it to emit bare-metal code works for tiny
> examples but becomes a source of subtle bugs as the OS grows. The `i386-elf`
> cross-compiler carries none of those assumptions and is the solid foundation
> for OS development.

## Toolchain setup (one-time)

Builds binutils + gcc for the `i386-elf` target into `~/opt/cross`:

```sh
# 1) prerequisites (Debian/Ubuntu)
sudo apt update
sudo apt install build-essential bison flex texinfo \
     libgmp3-dev libmpc-dev libmpfr-dev libisl-dev wget

# 2) build the toolchain (~30-60 min)
bash tools/build-cross-toolchain.sh

# 3) load it into PATH
source ~/.bashrc

# 4) verify
i386-elf-gcc --version
```

## Build and run

```sh
make             # build everything -> build/ and os.bin
make run         # build and run in qemu
make test-headless  # build and verify boot + apps headlessly
make clean       # remove build/ and os.bin
```

`make test-headless` boots the image in qemu, reads the serial log and injects
keystrokes through the monitor to verify the whole path: protected mode, video,
interrupts, the menu, launching isolated apps, and returning to the menu.

Helper targets: `make hex` (hexdump of the image), `make dall` (disassemble).

## Roadmap

- [x] 32-bit protected mode (LBA load, A20, GDT).
- [x] VBE linear framebuffer, font and text rendering.
- [x] Interrupts: IDT, PIC remap, PIT timer, PS/2 keyboard.
- [x] Paging and a physical frame allocator.
- [x] Userland (Ring 3): TSS, isolated address space, on-disk app loader.
- [x] Syscall interface; apps in assembly and C; fault isolation.
- [x] Start menu launching apps and games.
- [ ] ELF program loading.
- [ ] A real filesystem (replacing fixed raw slots).
- [ ] Sound (PC speaker) and more games.
