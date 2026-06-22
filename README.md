# StinkOS

A minimalist x86 operating system, written from scratch, whose goal is to **boot
into a start menu and run selectable games from it**. The aim is a small,
standardized runtime where games share a uniform size and are easy to list,
pick, and play straight from the menu.

The project is built with no external dependencies: bootloader, kernel, drivers,
and libraries are all written inside the repository.

## Components

| File         | Role |
|--------------|------|
| `boot.s`     | Assembly bootloader (16-bit real mode). Initializes the machine, loads the kernel from disk, and jumps into it. |
| `kernel.c`   | Kernel. Currently prints text via BIOS; it will grow to hold the drivers and the infrastructure that runs the games. |
| `Makefile`   | Build automation: assembles `boot.s`, compiles `kernel.c`, links everything into `os.bin` (output in `build/`). |
| `tools/`     | Setup utilities (e.g. the cross-compiler build script). |
| `CLAUDE.md`  | Development rules for the project. |

## Architecture and tooling

- Target: **x86** (i386), starting in 16-bit real mode.
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
make            # build everything -> build/ and os.bin
make run        # build and run in qemu
make clean      # remove build/ and os.bin
```

Helper targets: `make hex` (hexdump of the image), `make dboot` / `make dkernel`
/ `make dall` (objdump disassembly).

## Roadmap

- [ ] Finish the **video** (`screen.h`) and **IO** (`io.h`) drivers/libraries.
- [ ] Move the boot path from 16-bit real mode to **32-bit protected mode** (GDT).
- [ ] Lay out the `os.bin` image that holds the whole system.
- [ ] Start menu for game selection.
- [ ] Plan the game engine integrated into the kernel.
