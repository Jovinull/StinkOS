# StinkOS

A real, standard x86 PC operating system, written from scratch. StinkOS targets
**32-bit protected mode** with a proper split between **kernel space** and
**userland (Ring 3)**: processes, isolation, and a **syscall** contract.

Games, a browser, and other apps are built inside this project but run as
**independent userland binaries** — not compiled into the kernel. The kernel
stays lean: it loads those binaries from disk and runs them in Ring 3 through a
loader.

The project is built with no external dependencies: bootloader, kernel, drivers,
and libraries are all written inside the repository.

## Components

| File         | Role |
|--------------|------|
| `boot.s`     | Bootloader. Loads the kernel from disk (LBA), enables A20, sets up the GDT, switches to 32-bit protected mode, and jumps into the kernel. |
| `kernel.c`   | 32-bit kernel. Brings up a VGA text console and a serial debug console; it will grow to hold the drivers, the loader, and the userland infrastructure. |
| `Makefile`   | Build automation: assembles `boot.s`, compiles `kernel.c`, links everything into `os.bin` (output in `build/`). |
| `tools/`     | Setup utilities (e.g. the cross-compiler build script). |
| `CLAUDE.md`  | Development rules for the project. |

## Architecture and tooling

- Target: **x86** (i386), 32-bit protected mode (boots through real mode).
- Toolchain: a dedicated **`i386-elf` cross-compiler** (`i386-elf-as`,
  `i386-elf-gcc`, `i386-elf-ld`) — a compiler built for bare metal, not the host
  `gcc`.
- Emulator: `qemu-system-i386`.

> Why a cross-compiler and not the system `gcc`? The host `gcc` assumes it is
> producing programs for Linux. Forcing it to emit bare-metal code works for tiny
> examples but becomes a source of subtle bugs as the OS grows (protected mode,
> video drivers, games). The `i386-elf` cross-compiler carries none of those
> assumptions and is the solid foundation for OS development.

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
make test-headless  # build and verify the boot via the serial console
make clean       # remove build/ and os.bin
```

Helper targets: `make hex` (hexdump of the image), `make dboot` / `make dkernel`
/ `make dall` (objdump disassembly).

## Roadmap

- [x] Switch the boot path to **32-bit protected mode** (LBA load, A20, GDT).
- [x] VGA text console and serial debug console.
- [ ] Set up the video mode via **VBE** (real mode) before entering protected mode.
- [ ] Interrupts: **IDT**, PIC remap, timer and keyboard handlers.
- [ ] Memory management: paging and a kernel allocator.
- [ ] **Userland (Ring 3)**: TSS, a loader for on-disk binaries, and the **syscall** interface.
- [ ] Start menu and the first userland apps/games.
