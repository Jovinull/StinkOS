BUILD = build

# Default target if `make` is run with no args.
.DEFAULT_GOAL := help

help:
	@echo "StinkOS build targets:"
	@echo "  make all           -- build the kernel + every userland .elf"
	@echo "  make run           -- boot the freshly-built image in QEMU"
	@echo "  make run-install   -- boot the installer, copies to target.bin"
	@echo "  make run-installed -- boot target.bin (the post-install disk)"
	@echo "  make run-iso       -- boot stinkos-install.iso (USB-stick layout)"
	@echo "  make test-headless -- boot, run, capture serial, diff a baseline"
	@echo "  make unittest      -- run every host-side regression test"
	@echo "  make sample-packages -- build the .stinkpkg samples used by stink-pkg"
	@echo "  make stinkos-install.iso -- raw disk image (dd to a USB stick)"
	@echo "  make clean         -- delete build/ os.bin stinkos-install.iso"
	@echo "  make audit         -- run shell-side lint scripts on the source tree"

CC = i386-elf-gcc
AS = i386-elf-as
LD = i386-elf-ld
# Used by diagnostic targets (readelf-kernel) that invoke other binutils tools.
BUILD_TOOL_PREFIX = i386-elf-
CFLAGS = -Os -m32 -ffreestanding -fno-pie -fno-stack-protector -Wall -Wextra \
         -ffunction-sections -fdata-sections \
         -fmerge-all-constants -fno-asynchronous-unwind-tables -fno-unwind-tables \
         -fno-strict-aliasing \
         -Ilib
# -fmerge-all-constants: dedup string literals across translation units.
# -fno-asynchronous-unwind-tables -fno-unwind-tables: drop the C++ exception
#   tables (.eh_frame, .eh_frame_hdr) we never use -- the kernel is pure C.
# -flto: link-time optimisation. Compile passes emit LLVM-style bytecode
#   instead of (or in addition to) machine code; the link step (driven by
#   gcc so lto-plugin runs) re-runs optimisation across translation units
#   so it can inline helpers, drop dead branches and reuse common tails
#   that per-file builds leave on the table.
# -fno-strict-aliasing: belt-and-suspenders for cross-file inlining --
#   the kernel net code type-puns through `struct *` casts that the
#   strict-aliasing analysis would otherwise miscompile under -flto.

# -Ilib above puts the userland C library (lib/libstink.h) on the include path
# so apps keep using bare `#include "libstink.h"` after the move out of apps/.
# The kernel never includes libstink, so the extra -I is a harmless no-op there.

# Kernel source tree. Sources live under boot/ and kernel/<subsystem>/; objects
# stay flat in build/ (basenames are unique across the tree). VPATH lets the
# pattern rules resolve a bare source name to whichever subdirectory holds it,
# and KINCLUDES puts every kernel header dir on the include path so the existing
# bare `#include "foo.h"` lines keep resolving without per-file edits.
VPATH = boot kernel kernel/arch kernel/drivers/video kernel/drivers/input \
        kernel/drivers/storage kernel/drivers/audio kernel/drivers/net \
        kernel/drivers/misc kernel/fs kernel/sys kernel/ui
KINCLUDES = -Ikernel -Ikernel/arch -Ikernel/drivers/video -Ikernel/drivers/input \
            -Ikernel/drivers/storage -Ikernel/drivers/audio -Ikernel/drivers/net \
            -Ikernel/drivers/misc -Ikernel/fs -Ikernel/sys -Ikernel/ui
# App link flags: omagic (-N) packs the loadable segment with no page-alignment
# gap, and -s strips the symbol tables, keeping each app ELF down to one sector.
# Apps are a single flat code+data region (one set of user pages), so the load
# segment is intentionally RWX; silence ld's advisory warning about that.
# --gc-sections drops any function/data section the entry point can't reach,
# which (paired with -ffunction-sections / -fdata-sections above) means an app
# only pays the on-disk cost of the libstink helpers it actually calls.
APP_LDFLAGS = -T apps/app.ld -N -s --no-warn-rwx-segments --gc-sections

# Userland support library objects: malloc/free allocator, snprintf family,
# FILE*-based stdio, POSIX glue (errno, gettimeofday, open/stat...) and the
# setjmp/longjmp asm. Linked into every C app; --gc-sections at link time
# drops any unreferenced helpers so unused pieces cost nothing.
LIBSTINK_OBJS = $(BUILD)/libstink_alloc.o $(BUILD)/libstink_printf.o \
                $(BUILD)/libstink_stdio.o $(BUILD)/libstink_posix.o \
                $(BUILD)/libstink_setjmp.o $(BUILD)/libstink_http.o \
                $(BUILD)/libstink_sha256.o $(BUILD)/libstink_socket.o \
                $(BUILD)/libstink_math.o $(BUILD)/libstink_gfx.o \
                $(BUILD)/libstink_syms.o

# Doom port build configuration. The doomgeneric core wants the POSIX-ish
# headers our doom-shims provide; -DNORMALUNIX / -DLINUX picks the Chocolate
# Doom code path closest to a generic 32-bit Unix. -O2 is necessary -- without
# it Doom's renderer drops below playable frame rate -- but it is scoped to
# Doom only; libstink and the kernel stay at -O0. -w silences the wall of
# legacy-K&R warnings; -fno-strict-aliasing keeps the original type-punning
# code (Doom is famously not strict-aliasing-clean) functioning correctly.
DOOM_DIR    = apps/doom
DOOM_SHIMS  = apps/doom-shims
DOOM_CFLAGS = -O2 -m32 -ffreestanding -fno-pie -fno-stack-protector \
              -ffunction-sections -fdata-sections \
              -fno-builtin -fno-strict-aliasing \
              -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE -DFEATURE_SOUND \
              -I $(DOOM_SHIMS) -I lib -I apps -I $(DOOM_DIR) \
              -w

# Doom translation units. Mirrors doomgeneric's Makefile.soso minus the SDL,
# X11, Allegro and Emscripten backends we don't link in. doomgeneric_stink.c
# is our own platform layer (apps/doom/doomgeneric_stink.c).
DOOM_SRCS = dummy.c am_map.c doomdef.c doomstat.c dstrings.c d_event.c \
            d_items.c d_iwad.c d_loop.c d_main.c d_mode.c d_net.c \
            f_finale.c f_wipe.c g_game.c hu_lib.c hu_stuff.c info.c \
            i_cdmus.c i_endoom.c i_input.c i_joystick.c i_scale.c \
            i_system.c i_timer.c i_video.c \
            memio.c m_argv.c m_bbox.c m_cheat.c m_config.c m_controls.c \
            m_fixed.c m_menu.c m_misc.c m_random.c \
            p_ceilng.c p_doors.c p_enemy.c p_floor.c p_inter.c p_lights.c \
            p_map.c p_maputl.c p_mobj.c p_plats.c p_pspr.c p_saveg.c \
            p_setup.c p_sight.c p_spec.c p_switch.c p_telept.c p_tick.c \
            p_user.c \
            r_bsp.c r_data.c r_draw.c r_main.c r_plane.c r_segs.c r_sky.c \
            r_things.c \
            sha1.c sounds.c statdump.c st_lib.c st_stuff.c s_sound.c \
            tables.c v_video.c wi_stuff.c \
            w_checksum.c w_file.c w_main.c w_wad.c w_file_stdc.c \
            z_zone.c \
            doomgeneric.c doomgeneric_stink.c i_sound_stink.c

DOOM_OBJS = $(addprefix $(BUILD)/doom/, $(DOOM_SRCS:.c=.o))

# The Doom platform layer is variant-specific (one ELF per IWAD), so its .o
# is built three times with different -DSTINKDOOM_IWAD. The rest of the engine
# (DOOM_CORE_OBJS) is shared across all three variants.
DOOM_CORE_OBJS = $(filter-out $(BUILD)/doom/doomgeneric_stink.o, $(DOOM_OBJS))

# TODO §13 disk layout (ELF-aware bootloader):
#   LBA 0..15           bootblock     (boot.s + bootmain.o, flat binary,
#                                      linked at 0x7C00 by boot/bootblock.ld)
#   LBA 16..511         kernel.elf    (stripped, linked at 0x100000 by
#                                      boot/kernel.ld; 496 sectors = 248 KiB
#                                      headroom above the ~140 KiB kernel today)
#   LBA 512..513        StinkFS dir
#   LBA 514..200513     StinkFS data  (~100 MiB)
#
# Must stay in lock-step:
#   - BOOTBLOCK_SECTORS in boot/boot.s
#   - KERNEL_LBA       in boot/bootmain.c
#   - FS_DIR_LBA/FS_DATA_LBA/FS_DATA_END in kernel/fs/fs.c
BOOTBLOCK_SECTORS = 16
KERNEL_LBA        = 16
FS_DIR_LBA        = 512
FS_DATA_LBA       = 516
FS_DATA_END       = 200516
IMG_MIN           = 262144      # FS_DIR_LBA * 512 -- floor before make-stinkfs
DISK_END          = 102663168   # FS_DATA_END * 512

# WAD bundling. Defaults look under wads/ for the three Freedoom releases the
# fetch-wads.sh script downloads; override on the command line to point at a
# specific WAD or to disable a slot. Missing files are skipped silently.
FREEDOOM1_WAD ?= wads/freedoom1.wad
FREEDOOM2_WAD ?= wads/freedoom2.wad
FREEDM_WAD    ?= wads/freedm.wad

C_SRCS  = main.c serial.c trap.c syscall.c proc.c pipe.c timer.c klog.c bootdiag.c keyboard.c vbe.c fb.c font.c pmm.c paging.c cpuid.c acpi.c gdt.c ata.c elf.c speaker.c fs.c vfs.c menu.c mouse.c rtc.c audio.c dma.c pci.c e1000.c net.c ethernet.c arp.c ipv4.c icmp.c udp.c dhcp.c dns.c tcp.c mbr.c
C_OBJS  = $(addprefix $(BUILD)/, $(C_SRCS:.c=.o))

# Bootblock: boot.s + bootmain.o, linked at 0x7C00 as a flat binary.
# Sector 0 (boot.s real-mode prologue) is BIOS-loaded by definition; the
# follow-on sectors hold pm_entry + bootmain code.
BOOTBLOCK_OBJS = $(BUILD)/boot.o $(BUILD)/bootmain.o

# Kernel: multiboot header FIRST (so GRUB sees it within 8 KiB of file start),
# then kentry (sets segments + stack + zeros .bss + calls kmain), then the
# rest. Linked at 0x100000 as ELF; bootmain.c walks its PT_LOAD entries.
KERNEL_OBJS = $(BUILD)/multiboot.o $(BUILD)/kentry.o $(BUILD)/interrupts_asm.o $(BUILD)/gdt_asm.o $(BUILD)/usermode_asm.o $(BUILD)/context_asm.o $(C_OBJS)

all: os

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) $(KINCLUDES) -c $< -o $@

$(BUILD)/%.o: %.s | $(BUILD)
	$(AS) -O0 $< -o $@

# Userland support library: compiled once, linked into every C app.
$(BUILD)/libstink_alloc.o: lib/libstink_alloc.c lib/libstink.h | $(BUILD)
	$(CC) $(CFLAGS) -c lib/libstink_alloc.c -o $(BUILD)/libstink_alloc.o

# Non-inline shims for every libstink syscall wrapper. Existing C apps
# inline through libstink.h and never reference these symbols (so
# --gc-sections drops them); Rust apps import them via `extern "C"`
# because Rust cannot reach into a C inline header.
$(BUILD)/libstink_syms.o: lib/libstink_syms.c | $(BUILD)
	$(CC) $(CFLAGS) -c lib/libstink_syms.c -o $(BUILD)/libstink_syms.o

$(BUILD)/libstink_printf.o: lib/libstink_printf.c lib/libstink.h | $(BUILD)
	$(CC) $(CFLAGS) -c lib/libstink_printf.c -o $(BUILD)/libstink_printf.o

$(BUILD)/libstink_stdio.o: lib/libstink_stdio.c lib/libstink.h | $(BUILD)
	$(CC) $(CFLAGS) -c lib/libstink_stdio.c -o $(BUILD)/libstink_stdio.o

# POSIX glue needs the doom-shims headers on its include path so its <time.h>
# and <sys/stat.h> includes find the layout it implements against.
$(BUILD)/libstink_posix.o: lib/libstink_posix.c lib/libstink.h apps/doom-shims/time.h apps/doom-shims/sys/stat.h | $(BUILD)
	$(CC) $(CFLAGS) -I apps -I apps/doom-shims -c lib/libstink_posix.c -o $(BUILD)/libstink_posix.o

$(BUILD)/libstink_setjmp.o: lib/libstink_setjmp.s | $(BUILD)
	$(AS) -O0 lib/libstink_setjmp.s -o $(BUILD)/libstink_setjmp.o

$(BUILD)/libstink_http.o: lib/libstink_http.c lib/libstink.h lib/libstink_http.h | $(BUILD)
	$(CC) $(CFLAGS) -c lib/libstink_http.c -o $(BUILD)/libstink_http.o

$(BUILD)/libstink_sha256.o: lib/libstink_sha256.c | $(BUILD)
	$(CC) $(CFLAGS) -c lib/libstink_sha256.c -o $(BUILD)/libstink_sha256.o

$(BUILD)/libstink_socket.o: lib/libstink_socket.c lib/libstink.h lib/libstink_socket.h | $(BUILD)
	$(CC) $(CFLAGS) -c lib/libstink_socket.c -o $(BUILD)/libstink_socket.o

$(BUILD)/libstink_math.o: lib/libstink_math.c lib/libstink.h | $(BUILD)
	$(CC) $(CFLAGS) -c lib/libstink_math.c -o $(BUILD)/libstink_math.o

$(BUILD)/libstink_gfx.o: lib/libstink_gfx.c lib/libstink.h | $(BUILD)
	$(CC) $(CFLAGS) -c lib/libstink_gfx.c -o $(BUILD)/libstink_gfx.o

# Userland apps: ELF executables linked at the user code address (0x400000),
# loaded and relocated into the user region at runtime by the kernel ELF loader.
$(BUILD)/hello.elf: apps/hello.s apps/app.ld | $(BUILD)
	$(AS) -O0 apps/hello.s -o $(BUILD)/hello.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/hello.elf $(BUILD)/hello.o

$(BUILD)/box.elf: apps/box.s apps/app.ld | $(BUILD)
	$(AS) -O0 apps/box.s -o $(BUILD)/box.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/box.elf $(BUILD)/box.o

$(BUILD)/fault.elf: apps/fault.s apps/app.ld | $(BUILD)
	$(AS) -O0 apps/fault.s -o $(BUILD)/fault.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/fault.elf $(BUILD)/fault.o

$(BUILD)/game.elf: apps/game.s apps/app.ld | $(BUILD)
	$(AS) -O0 apps/game.s -o $(BUILD)/game.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/game.elf $(BUILD)/game.o

# C userland apps: crt0 (entry) linked first, then the compiled C object, then
# the shared libstink support objects.
$(BUILD)/hi.elf: apps/crt0.s apps/hi.c lib/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/hi.c -o $(BUILD)/hi_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/hi.elf $(BUILD)/crt0.o $(BUILD)/hi_app.o $(LIBSTINK_OBJS)

$(BUILD)/wxattack.elf: apps/crt0.s apps/wxattack.c lib/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/wxattack.c -o $(BUILD)/wxattack_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/wxattack.elf $(BUILD)/crt0.o $(BUILD)/wxattack_app.o $(LIBSTINK_OBJS)

$(BUILD)/cowtest.elf: apps/crt0.s apps/cowtest.c lib/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/cowtest.c -o $(BUILD)/cowtest_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/cowtest.elf $(BUILD)/crt0.o $(BUILD)/cowtest_app.o $(LIBSTINK_OBJS)

$(BUILD)/mounttest.elf: apps/crt0.s apps/mounttest.c lib/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/mounttest.c -o $(BUILD)/mounttest_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/mounttest.elf $(BUILD)/crt0.o $(BUILD)/mounttest_app.o $(LIBSTINK_OBJS)

$(BUILD)/exitcode.elf: apps/crt0.s apps/exitcode.c lib/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/exitcode.c -o $(BUILD)/exitcode_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/exitcode.elf $(BUILD)/crt0.o $(BUILD)/exitcode_app.o $(LIBSTINK_OBJS)

# Rust userland (v0.9+). Cargo is invoked through the user's standard
# rustup install (~/.cargo/bin); add it to PATH if your shell hasn't.
# Custom target spec apps/rust/i686-stinkos.json + -Z build-std=core
# means rustc compiles `core` from source for our bare-metal target;
# requires nightly toolchain + rust-src component (install with
# `rustup toolchain install nightly --component rust-src`).
#
# The Rust crate is a `staticlib` (.a) that exports `main` as extern
# "C"; the same C crt0.s used for every other app calls into it. All
# syscalls come from $(BUILD)/libstink_syms.o (non-inline shims so
# Rust's extern "C" can resolve them).
RUST_TARGET_JSON = apps/rust/i686-stinkos.json
CARGO ?= cargo
CARGO_FLAGS = +nightly build --release \
              --target $(RUST_TARGET_JSON) \
              -Z build-std=core,alloc \
              -Z unstable-options \
              -Z json-target-spec \
              --target-dir $(BUILD)/rust

$(BUILD)/rs-hello.elf: apps/crt0.s apps/rust/rs-hello/Cargo.toml \
                      apps/rust/rs-hello/src/lib.rs $(RUST_TARGET_JSON) \
                      apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CARGO) $(CARGO_FLAGS) --manifest-path apps/rust/rs-hello/Cargo.toml
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/rs-hello.elf $(BUILD)/crt0.o \
	      --whole-archive $(BUILD)/rust/i686-stinkos/release/librs_hello.a \
	      --no-whole-archive \
	      $(LIBSTINK_OBJS) $(LIBGCC)

$(BUILD)/rs-alloc.elf: apps/crt0.s apps/rust/rs-alloc/Cargo.toml \
                          apps/rust/rs-alloc/src/lib.rs $(RUST_TARGET_JSON) \
                          apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CARGO) $(CARGO_FLAGS) --manifest-path apps/rust/rs-alloc/Cargo.toml
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/rs-alloc.elf $(BUILD)/crt0.o \
	      --whole-archive $(BUILD)/rust/i686-stinkos/release/librs_alloc.a \
	      --no-whole-archive \
	      $(LIBSTINK_OBJS) $(LIBGCC)

$(BUILD)/rs-stdio.elf: apps/crt0.s apps/rust/rs-stdio/Cargo.toml \
                      apps/rust/rs-stdio/src/lib.rs \
                      apps/rust/libstink/Cargo.toml apps/rust/libstink/src/lib.rs \
                      $(RUST_TARGET_JSON) apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CARGO) $(CARGO_FLAGS) --manifest-path apps/rust/rs-stdio/Cargo.toml
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/rs-stdio.elf $(BUILD)/crt0.o \
	      --whole-archive $(BUILD)/rust/i686-stinkos/release/librs_stdio.a \
	      --no-whole-archive \
	      $(LIBSTINK_OBJS) $(LIBGCC)

$(BUILD)/rs-json.elf: apps/crt0.s apps/rust/rs-json/Cargo.toml \
                      apps/rust/rs-json/src/lib.rs \
                      apps/rust/libstink/Cargo.toml apps/rust/libstink/src/lib.rs \
                      $(RUST_TARGET_JSON) apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CARGO) $(CARGO_FLAGS) --manifest-path apps/rust/rs-json/Cargo.toml
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/rs-json.elf $(BUILD)/crt0.o \
	      --whole-archive $(BUILD)/rust/i686-stinkos/release/librs_json.a \
	      --no-whole-archive \
	      $(LIBSTINK_OBJS) $(LIBGCC)

$(BUILD)/rs-life.elf: apps/crt0.s apps/rust/rs-life/Cargo.toml \
                      apps/rust/rs-life/src/lib.rs \
                      apps/rust/libstink/Cargo.toml apps/rust/libstink/src/lib.rs \
                      $(RUST_TARGET_JSON) apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CARGO) $(CARGO_FLAGS) --manifest-path apps/rust/rs-life/Cargo.toml
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/rs-life.elf $(BUILD)/crt0.o \
	      --whole-archive $(BUILD)/rust/i686-stinkos/release/librs_life.a \
	      --no-whole-archive \
	      $(LIBSTINK_OBJS) $(LIBGCC)

$(BUILD)/rs-desktop.elf: apps/crt0.s apps/rust/rs-desktop/Cargo.toml \
                         apps/rust/rs-desktop/src/lib.rs \
                         apps/rust/libstink/Cargo.toml apps/rust/libstink/src/lib.rs \
                         apps/rust/libui/Cargo.toml apps/rust/libui/src/lib.rs \
                         $(RUST_TARGET_JSON) apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CARGO) $(CARGO_FLAGS) --manifest-path apps/rust/rs-desktop/Cargo.toml
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/rs-desktop.elf $(BUILD)/crt0.o \
	      --whole-archive $(BUILD)/rust/i686-stinkos/release/librs_desktop.a \
	      --no-whole-archive \
	      $(LIBSTINK_OBJS) $(LIBGCC)

$(BUILD)/rs-sysinfo.elf: apps/crt0.s apps/rust/rs-sysinfo/Cargo.toml \
                          apps/rust/rs-sysinfo/src/lib.rs \
                          apps/rust/libstink/Cargo.toml apps/rust/libstink/src/lib.rs \
                          apps/rust/libui/Cargo.toml apps/rust/libui/src/lib.rs \
                          $(RUST_TARGET_JSON) apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CARGO) $(CARGO_FLAGS) --manifest-path apps/rust/rs-sysinfo/Cargo.toml
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/rs-sysinfo.elf $(BUILD)/crt0.o \
	      --whole-archive $(BUILD)/rust/i686-stinkos/release/librs_sysinfo.a \
	      --no-whole-archive \
	      $(LIBSTINK_OBJS) $(LIBGCC)

$(BUILD)/rs-files.elf: apps/crt0.s apps/rust/rs-files/Cargo.toml \
                        apps/rust/rs-files/src/lib.rs \
                        apps/rust/libstink/Cargo.toml apps/rust/libstink/src/lib.rs \
                        apps/rust/libui/Cargo.toml apps/rust/libui/src/lib.rs \
                        $(RUST_TARGET_JSON) apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CARGO) $(CARGO_FLAGS) --manifest-path apps/rust/rs-files/Cargo.toml
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/rs-files.elf $(BUILD)/crt0.o \
	      --whole-archive $(BUILD)/rust/i686-stinkos/release/librs_files.a \
	      --no-whole-archive \
	      $(LIBSTINK_OBJS) $(LIBGCC)

$(BUILD)/rs-calc.elf: apps/crt0.s apps/rust/rs-calc/Cargo.toml \
                       apps/rust/rs-calc/src/lib.rs \
                       apps/rust/libstink/Cargo.toml apps/rust/libstink/src/lib.rs \
                       apps/rust/libui/Cargo.toml apps/rust/libui/src/lib.rs \
                       $(RUST_TARGET_JSON) apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CARGO) $(CARGO_FLAGS) --manifest-path apps/rust/rs-calc/Cargo.toml
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/rs-calc.elf $(BUILD)/crt0.o \
	      --whole-archive $(BUILD)/rust/i686-stinkos/release/librs_calc.a \
	      --no-whole-archive \
	      $(LIBSTINK_OBJS) $(LIBGCC)

$(BUILD)/rs-settings.elf: apps/crt0.s apps/rust/rs-settings/Cargo.toml \
                           apps/rust/rs-settings/src/lib.rs \
                           apps/rust/libstink/Cargo.toml apps/rust/libstink/src/lib.rs \
                           apps/rust/libui/Cargo.toml apps/rust/libui/src/lib.rs \
                           $(RUST_TARGET_JSON) apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CARGO) $(CARGO_FLAGS) --manifest-path apps/rust/rs-settings/Cargo.toml
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/rs-settings.elf $(BUILD)/crt0.o \
	      --whole-archive $(BUILD)/rust/i686-stinkos/release/librs_settings.a \
	      --no-whole-archive \
	      $(LIBSTINK_OBJS) $(LIBGCC)

$(BUILD)/rs-net.elf: apps/crt0.s apps/rust/rs-net/Cargo.toml \
                      apps/rust/rs-net/src/lib.rs \
                      apps/rust/libstink/Cargo.toml apps/rust/libstink/src/lib.rs \
                      apps/rust/libui/Cargo.toml apps/rust/libui/src/lib.rs \
                      $(RUST_TARGET_JSON) apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CARGO) $(CARGO_FLAGS) --manifest-path apps/rust/rs-net/Cargo.toml
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/rs-net.elf $(BUILD)/crt0.o \
	      --whole-archive $(BUILD)/rust/i686-stinkos/release/librs_net.a \
	      --no-whole-archive \
	      $(LIBSTINK_OBJS) $(LIBGCC)

$(BUILD)/rs-taskman.elf: apps/crt0.s apps/rust/rs-taskman/Cargo.toml \
                          apps/rust/rs-taskman/src/lib.rs \
                          apps/rust/libstink/Cargo.toml apps/rust/libstink/src/lib.rs \
                          apps/rust/libui/Cargo.toml apps/rust/libui/src/lib.rs \
                          $(RUST_TARGET_JSON) apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CARGO) $(CARGO_FLAGS) --manifest-path apps/rust/rs-taskman/Cargo.toml
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/rs-taskman.elf $(BUILD)/crt0.o \
	      --whole-archive $(BUILD)/rust/i686-stinkos/release/librs_taskman.a \
	      --no-whole-archive \
	      $(LIBSTINK_OBJS) $(LIBGCC)

$(BUILD)/rs-clock.elf: apps/crt0.s apps/rust/rs-clock/Cargo.toml \
                        apps/rust/rs-clock/src/lib.rs \
                        apps/rust/libstink/Cargo.toml apps/rust/libstink/src/lib.rs \
                        apps/rust/libui/Cargo.toml apps/rust/libui/src/lib.rs \
                        $(RUST_TARGET_JSON) apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CARGO) $(CARGO_FLAGS) --manifest-path apps/rust/rs-clock/Cargo.toml
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/rs-clock.elf $(BUILD)/crt0.o \
	      --whole-archive $(BUILD)/rust/i686-stinkos/release/librs_clock.a \
	      --no-whole-archive \
	      $(LIBSTINK_OBJS) $(LIBGCC)

$(BUILD)/rs-hex.elf: apps/crt0.s apps/rust/rs-hex/Cargo.toml \
                      apps/rust/rs-hex/src/lib.rs \
                      apps/rust/libstink/Cargo.toml apps/rust/libstink/src/lib.rs \
                      apps/rust/libui/Cargo.toml apps/rust/libui/src/lib.rs \
                      $(RUST_TARGET_JSON) apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CARGO) $(CARGO_FLAGS) --manifest-path apps/rust/rs-hex/Cargo.toml
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/rs-hex.elf $(BUILD)/crt0.o \
	      --whole-archive $(BUILD)/rust/i686-stinkos/release/librs_hex.a \
	      --no-whole-archive \
	      $(LIBSTINK_OBJS) $(LIBGCC)

$(BUILD)/rs-vec.elf: apps/crt0.s apps/rust/rs-vec/Cargo.toml \
                      apps/rust/rs-vec/src/lib.rs \
                      apps/rust/libstink/Cargo.toml apps/rust/libstink/src/lib.rs \
                      apps/rust/libui/Cargo.toml apps/rust/libui/src/lib.rs \
                      $(RUST_TARGET_JSON) apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CARGO) $(CARGO_FLAGS) --manifest-path apps/rust/rs-vec/Cargo.toml
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/rs-vec.elf $(BUILD)/crt0.o \
	      --whole-archive $(BUILD)/rust/i686-stinkos/release/librs_vec.a \
	      --no-whole-archive \
	      $(LIBSTINK_OBJS) $(LIBGCC)

$(BUILD)/rs-about.elf: apps/crt0.s apps/rust/rs-about/Cargo.toml \
                        apps/rust/rs-about/src/lib.rs \
                        apps/rust/libstink/Cargo.toml apps/rust/libstink/src/lib.rs \
                        apps/rust/libui/Cargo.toml apps/rust/libui/src/lib.rs \
                        $(RUST_TARGET_JSON) apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CARGO) $(CARGO_FLAGS) --manifest-path apps/rust/rs-about/Cargo.toml
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/rs-about.elf $(BUILD)/crt0.o \
	      --whole-archive $(BUILD)/rust/i686-stinkos/release/librs_about.a \
	      --no-whole-archive \
	      $(LIBSTINK_OBJS) $(LIBGCC)

$(BUILD)/rs-paint.elf: apps/crt0.s apps/rust/rs-paint/Cargo.toml \
                        apps/rust/rs-paint/src/lib.rs \
                        apps/rust/libstink/Cargo.toml apps/rust/libstink/src/lib.rs \
                        apps/rust/libui/Cargo.toml apps/rust/libui/src/lib.rs \
                        $(RUST_TARGET_JSON) apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CARGO) $(CARGO_FLAGS) --manifest-path apps/rust/rs-paint/Cargo.toml
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/rs-paint.elf $(BUILD)/crt0.o \
	      --whole-archive $(BUILD)/rust/i686-stinkos/release/librs_paint.a \
	      --no-whole-archive \
	      $(LIBSTINK_OBJS) $(LIBGCC)

$(BUILD)/rs-edit.elf: apps/crt0.s apps/rust/rs-edit/Cargo.toml \
                       apps/rust/rs-edit/src/lib.rs \
                       apps/rust/libstink/Cargo.toml apps/rust/libstink/src/lib.rs \
                       apps/rust/libui/Cargo.toml apps/rust/libui/src/lib.rs \
                       $(RUST_TARGET_JSON) apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CARGO) $(CARGO_FLAGS) --manifest-path apps/rust/rs-edit/Cargo.toml
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/rs-edit.elf $(BUILD)/crt0.o \
	      --whole-archive $(BUILD)/rust/i686-stinkos/release/librs_edit.a \
	      --no-whole-archive \
	      $(LIBSTINK_OBJS) $(LIBGCC)

$(BUILD)/anim.elf: apps/crt0.s apps/anim.c lib/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/anim.c -o $(BUILD)/anim_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/anim.elf $(BUILD)/crt0.o $(BUILD)/anim_app.o $(LIBSTINK_OBJS)

$(BUILD)/beep.elf: apps/crt0.s apps/beep.c lib/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/beep.c -o $(BUILD)/beep_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/beep.elf $(BUILD)/crt0.o $(BUILD)/beep_app.o $(LIBSTINK_OBJS)

$(BUILD)/save.elf: apps/crt0.s apps/save.c lib/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/save.c -o $(BUILD)/save_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/save.elf $(BUILD)/crt0.o $(BUILD)/save_app.o $(LIBSTINK_OBJS)

$(BUILD)/files.elf: apps/crt0.s apps/files.c lib/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/files.c -o $(BUILD)/files_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/files.elf $(BUILD)/crt0.o $(BUILD)/files_app.o $(LIBSTINK_OBJS)

$(BUILD)/ls.elf: apps/crt0.s apps/ls.c lib/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/ls.c -o $(BUILD)/ls_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/ls.elf $(BUILD)/crt0.o $(BUILD)/ls_app.o $(LIBSTINK_OBJS)

$(BUILD)/del.elf: apps/crt0.s apps/del.c lib/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/del.c -o $(BUILD)/del_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/del.elf $(BUILD)/crt0.o $(BUILD)/del_app.o $(LIBSTINK_OBJS)

$(BUILD)/play.elf: apps/crt0.s apps/play.c lib/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/play.c -o $(BUILD)/play_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/play.elf $(BUILD)/crt0.o $(BUILD)/play_app.o $(LIBSTINK_OBJS)

$(BUILD)/seek.elf: apps/crt0.s apps/seek.c lib/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/seek.c -o $(BUILD)/seek_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/seek.elf $(BUILD)/crt0.o $(BUILD)/seek_app.o $(LIBSTINK_OBJS)

$(BUILD)/fd.elf: apps/crt0.s apps/fd.c lib/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/fd.c -o $(BUILD)/fd_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/fd.elf $(BUILD)/crt0.o $(BUILD)/fd_app.o $(LIBSTINK_OBJS)

$(BUILD)/shell.elf: apps/crt0.s apps/shell.c lib/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/shell.c -o $(BUILD)/shell_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/shell.elf $(BUILD)/crt0.o $(BUILD)/shell_app.o $(LIBSTINK_OBJS)

$(BUILD)/arrows.elf: apps/crt0.s apps/arrows.c lib/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/arrows.c -o $(BUILD)/arrows_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/arrows.elf $(BUILD)/crt0.o $(BUILD)/arrows_app.o $(LIBSTINK_OBJS)

$(BUILD)/snake.elf: apps/crt0.s apps/snake.c lib/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/snake.c -o $(BUILD)/snake_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/snake.elf $(BUILD)/crt0.o $(BUILD)/snake_app.o $(LIBSTINK_OBJS)

$(BUILD)/pong.elf: apps/crt0.s apps/pong.c lib/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/pong.c -o $(BUILD)/pong_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/pong.elf $(BUILD)/crt0.o $(BUILD)/pong_app.o $(LIBSTINK_OBJS)

$(BUILD)/asteroids.elf: apps/crt0.s apps/asteroids.c lib/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/asteroids.c -o $(BUILD)/asteroids_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/asteroids.elf $(BUILD)/crt0.o $(BUILD)/asteroids_app.o $(LIBSTINK_OBJS)

$(BUILD)/installer.elf: apps/crt0.s apps/installer.c lib/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/installer.c -o $(BUILD)/installer_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/installer.elf $(BUILD)/crt0.o $(BUILD)/installer_app.o $(LIBSTINK_OBJS)

$(BUILD)/edit.elf: apps/crt0.s apps/edit.c lib/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/edit.c -o $(BUILD)/edit_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/edit.elf $(BUILD)/crt0.o $(BUILD)/edit_app.o $(LIBSTINK_OBJS)

$(BUILD)/fbdemo.elf: apps/crt0.s apps/fbdemo.c lib/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/fbdemo.c -o $(BUILD)/fbdemo_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/fbdemo.elf $(BUILD)/crt0.o $(BUILD)/fbdemo_app.o $(LIBSTINK_OBJS)

$(BUILD)/stinkpkg.elf: apps/crt0.s apps/stinkpkg.c apps/stinkpkg.h lib/libstink.h lib/libstink_http.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/stinkpkg.c -o $(BUILD)/stinkpkg_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/stinkpkg.elf $(BUILD)/crt0.o $(BUILD)/stinkpkg_app.o $(LIBSTINK_OBJS)

# Doom port: each translation unit compiles with DOOM_CFLAGS into its own
# build/doom/ subdir so the per-app objects don't collide with kernel objects
# in build/. The link is identical to the regular C-app pattern: crt0 first,
# all Doom objects, then the shared libstink helpers.
$(BUILD)/doom:
	mkdir -p $(BUILD)/doom

$(BUILD)/doom/%.o: $(DOOM_DIR)/%.c | $(BUILD)/doom
	$(CC) $(DOOM_CFLAGS) -c $< -o $@

# Per-variant builds of the platform layer: same source, one ELF per IWAD.
# Each picks a different file name to point doomgeneric at via -DSTINKDOOM_IWAD.
$(BUILD)/doom/stink_doom1.o: $(DOOM_DIR)/doomgeneric_stink.c | $(BUILD)/doom
	$(CC) $(DOOM_CFLAGS) -DSTINKDOOM_IWAD='"FREEDOOM1.WAD"' -c $< -o $@

$(BUILD)/doom/stink_doom2.o: $(DOOM_DIR)/doomgeneric_stink.c | $(BUILD)/doom
	$(CC) $(DOOM_CFLAGS) -DSTINKDOOM_IWAD='"FREEDOOM2.WAD"' -c $< -o $@

$(BUILD)/doom/stink_freedm.o: $(DOOM_DIR)/doomgeneric_stink.c | $(BUILD)/doom
	$(CC) $(DOOM_CFLAGS) -DSTINKDOOM_IWAD='"FREEDM.WAD"' -c $< -o $@

LIBGCC := $(shell i386-elf-gcc -print-libgcc-file-name)

$(BUILD)/doom1.elf: apps/crt0.s apps/app.ld $(DOOM_CORE_OBJS) $(BUILD)/doom/stink_doom1.o $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/doom1.elf $(BUILD)/crt0.o $(DOOM_CORE_OBJS) $(BUILD)/doom/stink_doom1.o $(LIBSTINK_OBJS) $(LIBGCC)

$(BUILD)/doom2.elf: apps/crt0.s apps/app.ld $(DOOM_CORE_OBJS) $(BUILD)/doom/stink_doom2.o $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/doom2.elf $(BUILD)/crt0.o $(DOOM_CORE_OBJS) $(BUILD)/doom/stink_doom2.o $(LIBSTINK_OBJS) $(LIBGCC)

$(BUILD)/freedm.elf: apps/crt0.s apps/app.ld $(DOOM_CORE_OBJS) $(BUILD)/doom/stink_freedm.o $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/freedm.elf $(BUILD)/crt0.o $(DOOM_CORE_OBJS) $(BUILD)/doom/stink_freedm.o $(LIBSTINK_OBJS) $(LIBGCC)

os: $(BOOTBLOCK_OBJS) $(KERNEL_OBJS) boot/bootblock.ld boot/kernel.ld $(BUILD)/hello.elf $(BUILD)/box.elf $(BUILD)/fault.elf $(BUILD)/game.elf $(BUILD)/hi.elf $(BUILD)/anim.elf $(BUILD)/beep.elf $(BUILD)/save.elf $(BUILD)/files.elf $(BUILD)/ls.elf $(BUILD)/del.elf $(BUILD)/play.elf $(BUILD)/seek.elf $(BUILD)/fd.elf $(BUILD)/shell.elf $(BUILD)/arrows.elf $(BUILD)/snake.elf $(BUILD)/pong.elf $(BUILD)/asteroids.elf $(BUILD)/installer.elf $(BUILD)/edit.elf $(BUILD)/fbdemo.elf $(BUILD)/stinkpkg.elf $(BUILD)/doom1.elf $(BUILD)/doom2.elf $(BUILD)/freedm.elf $(BUILD)/wxattack.elf $(BUILD)/cowtest.elf $(BUILD)/mounttest.elf $(BUILD)/rs-hello.elf $(BUILD)/rs-alloc.elf $(BUILD)/rs-stdio.elf $(BUILD)/rs-json.elf $(BUILD)/rs-life.elf $(BUILD)/rs-desktop.elf $(BUILD)/rs-sysinfo.elf $(BUILD)/rs-files.elf $(BUILD)/rs-calc.elf $(BUILD)/rs-settings.elf $(BUILD)/rs-net.elf $(BUILD)/rs-taskman.elf $(BUILD)/rs-clock.elf $(BUILD)/rs-hex.elf $(BUILD)/rs-vec.elf $(BUILD)/rs-about.elf $(BUILD)/rs-paint.elf $(BUILD)/rs-edit.elf $(BUILD)/exitcode.elf
	# --- 1. Link the bootblock (flat binary, sector 0 + bootmain code) ---
	$(LD) -T boot/bootblock.ld --oformat binary -o $(BUILD)/bootblock.bin $(BOOTBLOCK_OBJS)
	@bb=$$(stat -c%s $(BUILD)/bootblock.bin); max=$$(($(BOOTBLOCK_SECTORS) * 512)); \
		if [ $$bb -gt $$max ]; then \
			echo "ERROR: bootblock $$bb B > BOOTBLOCK_SECTORS * 512 = $$max B; raise BOOTBLOCK_SECTORS in boot/boot.s AND Makefile"; exit 1; fi
	# --- 2. Link the kernel as a real ELF at 0x100000 ---
	# libgcc.a supplies the 64-bit arithmetic helpers (__udivdi3, __umoddi3)
	# that audio_mix_play_rate's Q16.16 step computation needs. Pre-§13 we
	# avoided libgcc to stay under a 64 KiB bootloader cap; with ELF-aware
	# boot we have headroom and can use the natural 64-bit form.
	#
	# --gc-sections drops every function/data section the entry point can't
	# reach. Paired with -ffunction-sections / -fdata-sections (in CFLAGS)
	# it strips dead helpers libgcc dragged in plus any kernel symbols that
	# only fired in older code paths. Cheap insurance against bloat.
	$(LD) -T boot/kernel.ld --gc-sections --build-id=none -o $(BUILD)/kernel.elf $(KERNEL_OBJS) $(LIBGCC)
	@printf 'kernel sections   '; $(BUILD_TOOL_PREFIX)size $(BUILD)/kernel.elf | tail -1
	# --- 3. Strip section headers / symbols for on-disk image ---
	# kernel.elf is ~120 KiB unstripped (debug info, section headers);
	# stripped drops to ~75 KiB. bootmain only needs PT_LOAD program
	# headers + segment payloads, both preserved by strip.
	cp $(BUILD)/kernel.elf $(BUILD)/kernel.stripped.elf
	$(BUILD_TOOL_PREFIX)strip --strip-all $(BUILD)/kernel.stripped.elf
	@ksize=$$(stat -c%s $(BUILD)/kernel.stripped.elf); \
		room=$$(($(FS_DIR_LBA) * 512 - $(KERNEL_LBA) * 512)); \
		if [ $$ksize -gt $$room ]; then \
			echo "ERROR: stripped kernel.elf $$ksize B > kernel slot $$room B (LBA $(KERNEL_LBA)..$$(($(FS_DIR_LBA) - 1))); raise FS_DIR_LBA"; exit 1; fi
	# --- 4. Assemble os.bin: bootblock at LBA 0, kernel.elf at KERNEL_LBA ---
	cp $(BUILD)/bootblock.bin os.bin
	truncate -s $$(($(KERNEL_LBA) * 512)) os.bin
	cat $(BUILD)/kernel.stripped.elf >> os.bin
	@size=$$(stat -c%s os.bin); if [ $$size -lt $(IMG_MIN) ]; then truncate -s $(IMG_MIN) os.bin; fi
	@size=$$(stat -c%s os.bin); if [ $$size -lt $(DISK_END) ]; then truncate -s $(DISK_END) os.bin; fi
	@args="HELLO.ELF=$(BUILD)/hello.elf \
	  BOX.ELF=$(BUILD)/box.elf \
	  FAULT.ELF=$(BUILD)/fault.elf \
	  GAME.ELF=$(BUILD)/game.elf \
	  HI.ELF=$(BUILD)/hi.elf \
	  ANIM.ELF=$(BUILD)/anim.elf \
	  BEEP.ELF=$(BUILD)/beep.elf \
	  SAVE.ELF=$(BUILD)/save.elf \
	  FILES.ELF=$(BUILD)/files.elf \
	  LS.ELF=$(BUILD)/ls.elf \
	  DEL.ELF=$(BUILD)/del.elf \
	  PLAY.ELF=$(BUILD)/play.elf \
	  SEEK.ELF=$(BUILD)/seek.elf \
	  FD.ELF=$(BUILD)/fd.elf \
	  SHELL.ELF=$(BUILD)/shell.elf \
	  ARROWS.ELF=$(BUILD)/arrows.elf \
	  SNAKE.ELF=$(BUILD)/snake.elf \
	  PONG.ELF=$(BUILD)/pong.elf \
	  ASTEROIDS.ELF=$(BUILD)/asteroids.elf \
	  INSTALLER.ELF=$(BUILD)/installer.elf \
	  EDIT.ELF=$(BUILD)/edit.elf \
	  STINKPKG.ELF=$(BUILD)/stinkpkg.elf \
	  DOOM1.ELF=$(BUILD)/doom1.elf \
	  DOOM2.ELF=$(BUILD)/doom2.elf \
	  FREEDM.ELF=$(BUILD)/freedm.elf \
	  FBDEMO.ELF=$(BUILD)/fbdemo.elf \
	  WXATTACK.ELF=$(BUILD)/wxattack.elf \
	  COWTEST.ELF=$(BUILD)/cowtest.elf \
	  MOUNTTEST.ELF=$(BUILD)/mounttest.elf \
	  RS-HELLO.ELF=$(BUILD)/rs-hello.elf \
	  RS-ALLOC.ELF=$(BUILD)/rs-alloc.elf \
	  RS-STDIO.ELF=$(BUILD)/rs-stdio.elf \
	  RS-JSON.ELF=$(BUILD)/rs-json.elf \
	  TEST.JSON=apps/rust/rs-json/test.json \
	  RS-LIFE.ELF=$(BUILD)/rs-life.elf \
	  RS-DESKTOP.ELF=$(BUILD)/rs-desktop.elf \
	  RS-SYSINFO.ELF=$(BUILD)/rs-sysinfo.elf \
	  RS-FILES.ELF=$(BUILD)/rs-files.elf \
	  RS-CALC.ELF=$(BUILD)/rs-calc.elf \
	  RS-HEX.ELF=$(BUILD)/rs-hex.elf \
	  RS-VEC.ELF=$(BUILD)/rs-vec.elf \
	  RS-SETTINGS.ELF=$(BUILD)/rs-settings.elf \
	  RS-NET.ELF=$(BUILD)/rs-net.elf \
	  RS-TASKMAN.ELF=$(BUILD)/rs-taskman.elf \
	  RS-CLOCK.ELF=$(BUILD)/rs-clock.elf \
	  RS-ABOUT.ELF=$(BUILD)/rs-about.elf \
	  RS-PAINT.ELF=$(BUILD)/rs-paint.elf \
	  RS-EDIT.ELF=$(BUILD)/rs-edit.elf \
	  EXITCODE.ELF=$(BUILD)/exitcode.elf"; \
	  if [ -f "$(FREEDOOM1_WAD)" ]; then args="$$args FREEDOOM1.WAD=$(FREEDOOM1_WAD)"; fi; \
	  if [ -f "$(FREEDOOM2_WAD)" ]; then args="$$args FREEDOOM2.WAD=$(FREEDOOM2_WAD)"; fi; \
	  if [ -f "$(FREEDM_WAD)" ];    then args="$$args FREEDM.WAD=$(FREEDM_WAD)"; fi; \
	  python3 tools/make-stinkfs.py os.bin $(FS_DIR_LBA) $(FS_DATA_LBA) $(FS_DATA_END) $$args

hex:
	hexdump os.bin

dall:
	objdump -m i386 -b binary --adjust-vma=0x7c00 -D os.bin

# Inspect the kernel ELF's PT_LOAD program headers. Used to verify the
# ELF-aware bootloader prep (TODO §13): the kernel.elf MUST carry
# well-formed phdrs that boot/bootmain.c will walk. Fails fast if
# build/kernel.elf is missing.
readelf-kernel:
	@test -f $(BUILD)/kernel.elf || { echo "ERROR: $(BUILD)/kernel.elf missing; run \`make\` first"; exit 1; }
	@$(BUILD_TOOL_PREFIX)readelf -l $(BUILD)/kernel.elf
	@printf '\nELF total file size: '
	@stat -c%s $(BUILD)/kernel.elf
	@printf 'PT_LOAD bytes on disk (sum of filesz, what bootmain reads): '
	@$(BUILD_TOOL_PREFIX)readelf -lW $(BUILD)/kernel.elf | awk '/^  LOAD/ {s+=strtonum($$5)} END {print s}'

# QEMU audio backend for the Sound Blaster 16 device:
#   none  -- driver initialises, no sound reaches the host (default; quiet)
#   sdl   -- cross-platform SDL audio (Linux/macOS/Windows; needs SDL2 libs)
#   pa    -- PulseAudio (Linux)
#   dsound -- DirectSound (Windows host)
# Override per-run: make QEMU_AUDIO=sdl run
QEMU_AUDIO ?= none

run: all
	qemu-system-i386 -drive format=raw,file=os.bin \
	  -audiodev $(QEMU_AUDIO),id=snd0 \
	  -device sb16,audiodev=snd0 \
	  -netdev user,id=net0 -device e1000,netdev=net0

# Blank target disk for the installer app to clone the boot media onto.
# Created on demand; ignored by git (*.bin in .gitignore). 64 MiB is enough
# headroom for the full StinkOS image (~101 MiB will hit a "target too small"
# guard, but a few apps + WADs fit comfortably under 64).
INSTALL_TARGET_SIZE = 134217728     # 128 MiB
target.bin:
	truncate -s $(INSTALL_TARGET_SIZE) target.bin

# Boot the install media (drive 0) with a blank target (drive 2) attached.
# The boot menu's INSTALL entry clones os.bin sector-for-sector onto the
# target; after success, run-installed boots from the target alone.
run-install: all target.bin
	qemu-system-i386 \
	  -drive file=os.bin,format=raw,if=ide,index=0 \
	  -drive file=target.bin,format=raw,if=ide,index=2 \
	  -audiodev $(QEMU_AUDIO),id=snd0 -device sb16,audiodev=snd0 \
	  -netdev user,id=net0 -device e1000,netdev=net0

# Boot just the installed system (after run-install finished a clone).
run-installed: target.bin
	qemu-system-i386 -drive file=target.bin,format=raw \
	  -audiodev $(QEMU_AUDIO),id=snd0 -device sb16,audiodev=snd0 \
	  -netdev user,id=net0 -device e1000,netdev=net0

# Headless verification: boots the image in qemu, reads the serial debug log and
# injects keystrokes via the monitor to assert protected mode, the timer IRQ and
# the keyboard IRQ all work. See tools/test-headless.py.
test-headless: all
	@python3 tools/test-headless.py

# Multi-process smoke: boots, enters the shell, runs `bg anim` to confirm
# fork+exec hands the child a working ring-3 entry while the parent keeps
# serving the prompt. Decoupled from test-headless so it can fail / iterate
# independently without rerunning the 25 s graphical menu sweep.
smoke-multiproc: all
	@python3 tools/smoke-multiproc.py

# ACPI smoke: boots, opens the shell, runs `shutdown`, asserts the
# ACPI S5 write path fired and QEMU powered off. Decoupled from
# test-headless so it can validate the destructive shutdown path
# without aborting the broader test run.
smoke-acpi: all
	@python3 tools/smoke-acpi.py

# COW fork smoke: boots, opens the shell, runs `cowtest`, asserts the
# child + parent see the shared pre-fork marker AND privately diverge
# on subsequent writes (proves the COW #PF handler copied the frame).
smoke-cow: all
	@python3 tools/smoke-cow.py

# VFS multi-mount smoke: boots, opens the shell, runs `mounttest`, asserts
# sys_mount registers slot 1 (B:) + write + read roundtrip through the
# prefix-routed fs API matches the bytes written.
smoke-vfs-mounts: all
	@python3 tools/smoke-vfs-mounts.py

# Rust toolchain smoke: boots, opens the shell, runs `rs-hello`, asserts
# the Rust extern "C" main() called sys_log through the libstink_syms
# shims (proves the full toolchain pipeline: cargo + build-std + custom
# i686-stinkos target + libstink_syms linkage). Tier 1.1 milestone.
smoke-rs-hello: all
	@python3 tools/smoke-rs-hello.py

# Rust alloc crate smoke: drives the shell to launch `rs-alloc`,
# which exercises Box<T>, Vec<T> growth, and free-then-realloc reuse
# through libstink K&R malloc behind a #[global_allocator] shim. Tier
# 1.3 milestone.
smoke-rs-alloc: all
	@python3 tools/smoke-rs-alloc.py

# rs-json smoke: drives shell to `run rs-json` which reads TEST.JSON
# from StinkFS, parses + pretty-prints. Asserts every expected
# fragment of the round-trip output appears in serial.
smoke-rs-json: all
	@python3 tools/smoke-rs-json.py

# W^X adversarial smoke: split out of test-headless so CI runner slack
# doesn't push the assertion past its window. Drives shell to run
# wxattack ELF which writes 0xC3 to .data then function-pointer-calls
# it; v0.5 W^X NX bit fires #PF on the instruction fetch and the
# kernel kills the process.
smoke-wxattack: all
	@python3 tools/smoke-wxattack.py

# rs-life smoke (Conway's Game of Life): drives shell to launch the
# Rust visual app, lets it step ~250 generations, asserts the
# periodic log lines fire AND that alive counts evolve over time
# (proves step() rule is firing on the glider gun, not a static grid).
smoke-rs-life: all
	@python3 tools/smoke-rs-life.py

# Exit code smoke (Tier 2.3): asserts sys_exit_code(N) propagates to
# parent's sys_wait() return value.
smoke-exitcode: all
	@python3 tools/smoke-exitcode.py

# Rust stdio shim smoke (Tier 2.2): exercises println!/eprintln!/
# alloc::format! through the shared `libstink` Rust crate.
smoke-rs-stdio: all
	@python3 tools/smoke-rs-stdio.py

# Static analysis sweep over kernel + lib + apps. Runs whichever of cppcheck,
# clang-tidy and scan-build are installed -- skips missing tools quietly so
# the target stays useful in any environment, including CI. Vendored sources
# under apps/doom/ are excluded (they're third-party and famously not
# strict-alias clean; warnings there would drown out real findings).
AUDIT_SRCS = kernel boot lib $(filter-out apps/doom apps/doom-shims, $(wildcard apps))
audit:
	@echo "=== StinkOS static-analysis sweep ==="
	@if command -v cppcheck >/dev/null 2>&1; then \
	    echo "--- cppcheck ---"; \
	    cppcheck --quiet --enable=warning,style,performance,portability \
	             --inline-suppr --suppress=unusedFunction \
	             -I kernel -I kernel/arch -I kernel/drivers/video \
	             -I kernel/drivers/input -I kernel/drivers/storage \
	             -I kernel/drivers/audio -I kernel/drivers/net \
	             -I kernel/drivers/misc -I kernel/fs -I kernel/sys \
	             -I kernel/ui -I lib -I apps \
	             $(AUDIT_SRCS); \
	  else \
	    echo "--- cppcheck: not installed, skipping ---"; \
	  fi
	@if command -v clang-tidy >/dev/null 2>&1; then \
	    echo "--- clang-tidy ---"; \
	    find kernel lib -name '*.c' -print0 | \
	        xargs -0 -n 8 clang-tidy --quiet \
	            -checks='bugprone-*,clang-analyzer-*,-clang-analyzer-security.insecureAPI.*' \
	            -- -m32 -ffreestanding -nostdinc \
	               -Ikernel -Ikernel/arch -Ikernel/sys \
	               -Ikernel/drivers/video -Ikernel/drivers/input \
	               -Ikernel/drivers/storage -Ikernel/drivers/audio \
	               -Ikernel/drivers/net -Ikernel/drivers/misc \
	               -Ikernel/fs -Ikernel/ui -Ilib 2>/dev/null || true; \
	  else \
	    echo "--- clang-tidy: not installed, skipping ---"; \
	  fi
	@echo "=== audit done ==="

# Distribution image: a copy of os.bin with a proper single-partition MBR
# table patched into sector 0. The on-disk bytes are otherwise identical, so
# `dd` to a USB stick or `qemu -drive file=stinkos-install.iso` boots the
# same system as `make run`. Despite the .iso suffix this is a raw disk
# image, not ISO9660 -- the naming follows the convention every hobby OS
# distribution uses ("download the .iso, dd it to a stick").
INSTALL_PART_START   = 1
INSTALL_PART_SECTORS = $(shell echo $$(( $(FS_DATA_END) - 1 )))
stinkos-install.iso: all tools/write-mbr.py
	cp os.bin stinkos-install.iso
	python3 tools/write-mbr.py stinkos-install.iso \
	    --start $(INSTALL_PART_START) \
	    --count $(INSTALL_PART_SECTORS)

# Boot the published install image standalone. Verifies the ISO build works
# end-to-end without going through the regular os.bin path.
run-iso: stinkos-install.iso
	qemu-system-i386 -drive format=raw,file=stinkos-install.iso \
	  -audiodev $(QEMU_AUDIO),id=snd0 -device sb16,audiodev=snd0 \
	  -netdev user,id=net0 -device e1000,netdev=net0

# Sample .stinkpkg archives. Bundles a handful of built userland ELFs through
# tools/make-stinkpkg.py and drops them under repo/, ready to be served by
# tools/repo-server.py. Use after `make` so the .elf inputs exist.
SAMPLE_REPO = repo
sample-packages: all tools/make-stinkpkg.py
	@mkdir -p $(SAMPLE_REPO)
	python3 tools/make-stinkpkg.py \
	    --name edit  --version 0.1.0 --desc "Stink text editor" \
	    --out $(SAMPLE_REPO)/edit.stinkpkg  $(BUILD)/edit.elf
	python3 tools/make-stinkpkg.py \
	    --name snake --version 1.0.0 --desc "Snake game" \
	    --out $(SAMPLE_REPO)/snake.stinkpkg $(BUILD)/snake.elf
	python3 tools/make-stinkpkg.py \
	    --name pong  --version 1.0.0 --desc "Pong game" \
	    --out $(SAMPLE_REPO)/pong.stinkpkg  $(BUILD)/pong.elf
	python3 tools/make-stinkpkg.py \
	    --name asteroids --version 1.0.0 --desc "Asteroids game" \
	    --out $(SAMPLE_REPO)/asteroids.stinkpkg $(BUILD)/asteroids.elf
	python3 tools/make-stinkpkg.py \
	    --name hello --version 1.0.0 --desc "Minimal hello-world" \
	    --out $(SAMPLE_REPO)/hello.stinkpkg $(BUILD)/hi.elf
	@echo "sample packages written under $(SAMPLE_REPO)/"
	@echo "run: tools/repo-server.py --pkgdir $(SAMPLE_REPO)"

# Host-side unit tests. Compile + run pure-C subsystems (SHA-256, BSD inet
# helpers, ...) against the host gcc so we get a fast signal that does not
# need QEMU or the cross-toolchain. Lives under tests/.
HOST_CC  ?= gcc
HOST_CFLAGS = -O2 -Wall -Werror -Wno-unused-function \
              -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
              -Wno-builtin-declaration-mismatch
# Why -Wno-builtin-declaration-mismatch: libstink.h declares the libc shape
# (malloc/strlen/snprintf/...) with size_t == unsigned int because the
# StinkOS userland is i386 with no hosted libc. When the host gcc (x86_64)
# parses the same header for a unit test, its built-in libc declarations
# use unsigned long. The mismatch is real but structural -- the kernel
# can't be size_t-correct without dragging in a libc, and the test
# binaries don't actually call malloc/strlen via libstink (they include
# their own arithmetic). Suppressing the one warning lets -Werror keep
# its grip on every other category.
TEST_DIR  = tests
TEST_BIN  = $(BUILD)/tests

$(TEST_BIN):
	mkdir -p $(TEST_BIN)

$(TEST_BIN)/test_sha256: $(TEST_DIR)/test_sha256.c lib/libstink_sha256.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_sha256.c lib/libstink_sha256.c

$(TEST_BIN)/test_inet_addr: $(TEST_DIR)/test_inet_addr.c lib/libstink_socket.c lib/libstink_socket.h | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -I lib -o $@ $(TEST_DIR)/test_inet_addr.c lib/libstink_socket.c

$(TEST_BIN)/test_mixer: $(TEST_DIR)/test_mixer.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_mixer.c

$(TEST_BIN)/test_ipv4_checksum: $(TEST_DIR)/test_ipv4_checksum.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_ipv4_checksum.c

$(TEST_BIN)/test_tcp_options: $(TEST_DIR)/test_tcp_options.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_tcp_options.c

$(TEST_BIN)/test_sched: $(TEST_DIR)/test_sched.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_sched.c

$(TEST_BIN)/test_tcp_state: $(TEST_DIR)/test_tcp_state.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_tcp_state.c

$(TEST_BIN)/test_ipv4_parse: $(TEST_DIR)/test_ipv4_parse.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_ipv4_parse.c

$(TEST_BIN)/test_arp_cache: $(TEST_DIR)/test_arp_cache.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_arp_cache.c

$(TEST_BIN)/test_mbr_parse: $(TEST_DIR)/test_mbr_parse.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_mbr_parse.c

$(TEST_BIN)/test_dhcp_options: $(TEST_DIR)/test_dhcp_options.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_dhcp_options.c

$(TEST_BIN)/test_eth_frame: $(TEST_DIR)/test_eth_frame.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_eth_frame.c

$(TEST_BIN)/test_arp_storm: $(TEST_DIR)/test_arp_storm.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_arp_storm.c

$(TEST_BIN)/test_stinkfs_dir: $(TEST_DIR)/test_stinkfs_dir.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_stinkfs_dir.c

$(TEST_BIN)/test_tcp_dupack: $(TEST_DIR)/test_tcp_dupack.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_tcp_dupack.c

$(TEST_BIN)/test_tcp_sack_use: $(TEST_DIR)/test_tcp_sack_use.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_tcp_sack_use.c

$(TEST_BIN)/test_icmp_ratelimit: $(TEST_DIR)/test_icmp_ratelimit.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_icmp_ratelimit.c

$(TEST_BIN)/test_tcp_persist: $(TEST_DIR)/test_tcp_persist.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_tcp_persist.c

$(TEST_BIN)/test_tcp_timewait: $(TEST_DIR)/test_tcp_timewait.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_tcp_timewait.c

$(TEST_BIN)/test_dns_retry: $(TEST_DIR)/test_dns_retry.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_dns_retry.c

$(TEST_BIN)/test_dhcp_retry: $(TEST_DIR)/test_dhcp_retry.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_dhcp_retry.c

$(TEST_BIN)/test_blit_overflow: $(TEST_DIR)/test_blit_overflow.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_blit_overflow.c

$(TEST_BIN)/test_udp_checksum: $(TEST_DIR)/test_udp_checksum.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_udp_checksum.c

$(TEST_BIN)/test_tcp_checksum: $(TEST_DIR)/test_tcp_checksum.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_tcp_checksum.c

$(TEST_BIN)/test_tcp_rst_gate: $(TEST_DIR)/test_tcp_rst_gate.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_tcp_rst_gate.c

$(TEST_BIN)/test_tcp_rxwnd: $(TEST_DIR)/test_tcp_rxwnd.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_tcp_rxwnd.c

$(TEST_BIN)/test_tcp_syn_gate: $(TEST_DIR)/test_tcp_syn_gate.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_tcp_syn_gate.c

$(TEST_BIN)/test_ipv4_unicast: $(TEST_DIR)/test_ipv4_unicast.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_ipv4_unicast.c

$(TEST_BIN)/test_ipv4_srcroute: $(TEST_DIR)/test_ipv4_srcroute.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_ipv4_srcroute.c

$(TEST_BIN)/test_ipv4_martian: $(TEST_DIR)/test_ipv4_martian.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_ipv4_martian.c

$(TEST_BIN)/test_ipv4_teardrop: $(TEST_DIR)/test_ipv4_teardrop.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_ipv4_teardrop.c

$(TEST_BIN)/test_fb_rect_clip: $(TEST_DIR)/test_fb_rect_clip.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_fb_rect_clip.c

$(TEST_BIN)/test_mmap_overflow: $(TEST_DIR)/test_mmap_overflow.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_mmap_overflow.c

$(TEST_BIN)/test_blit_scale: $(TEST_DIR)/test_blit_scale.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_blit_scale.c

$(TEST_BIN)/test_tcp_keepalive: $(TEST_DIR)/test_tcp_keepalive.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_tcp_keepalive.c

$(TEST_BIN)/test_dns_cache: $(TEST_DIR)/test_dns_cache.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_dns_cache.c

$(TEST_BIN)/test_tcp_cwnd: $(TEST_DIR)/test_tcp_cwnd.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_tcp_cwnd.c

$(TEST_BIN)/test_tcp_wscale: $(TEST_DIR)/test_tcp_wscale.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_tcp_wscale.c

$(TEST_BIN)/test_tcp_ooo: $(TEST_DIR)/test_tcp_ooo.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_tcp_ooo.c

$(TEST_BIN)/test_ipv4_reasm_alloc: $(TEST_DIR)/test_ipv4_reasm_alloc.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_ipv4_reasm_alloc.c

$(TEST_BIN)/test_pipe: $(TEST_DIR)/test_pipe.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_pipe.c

$(TEST_BIN)/test_klog: $(TEST_DIR)/test_klog.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_klog.c

$(TEST_BIN)/test_timer: $(TEST_DIR)/test_timer.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_timer.c

$(TEST_BIN)/test_tcp_rto: $(TEST_DIR)/test_tcp_rto.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_tcp_rto.c

$(TEST_BIN)/test_fs_grow: $(TEST_DIR)/test_fs_grow.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_fs_grow.c

$(TEST_BIN)/test_fs_delete: $(TEST_DIR)/test_fs_delete.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_fs_delete.c

$(TEST_BIN)/test_elf_loader: $(TEST_DIR)/test_elf_loader.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_elf_loader.c

$(TEST_BIN)/test_keymap_br: $(TEST_DIR)/test_keymap_br.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_keymap_br.c

$(TEST_BIN)/test_vfs_fd: $(TEST_DIR)/test_vfs_fd.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_vfs_fd.c

$(TEST_BIN)/test_paging_brk: $(TEST_DIR)/test_paging_brk.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_paging_brk.c

$(TEST_BIN)/test_proc_reap: $(TEST_DIR)/test_proc_reap.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_proc_reap.c

$(TEST_BIN)/test_mouse_packet: $(TEST_DIR)/test_mouse_packet.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_mouse_packet.c

$(TEST_BIN)/test_rtc_alarm: $(TEST_DIR)/test_rtc_alarm.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_rtc_alarm.c

$(TEST_BIN)/test_tcp_tcb_sim: $(TEST_DIR)/test_tcp_tcb_sim.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_tcp_tcb_sim.c

$(TEST_BIN)/test_audio_mode: $(TEST_DIR)/test_audio_mode.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_audio_mode.c

$(TEST_BIN)/test_mbr_write: $(TEST_DIR)/test_mbr_write.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_mbr_write.c

$(TEST_BIN)/test_tcp_close_pid: $(TEST_DIR)/test_tcp_close_pid.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_tcp_close_pid.c

$(TEST_BIN)/test_icmp_echo: $(TEST_DIR)/test_icmp_echo.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_icmp_echo.c

$(TEST_BIN)/test_arp_ratelimit: $(TEST_DIR)/test_arp_ratelimit.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_arp_ratelimit.c

$(TEST_BIN)/test_utf8_collapse: $(TEST_DIR)/test_utf8_collapse.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_utf8_collapse.c

$(TEST_BIN)/test_dhcp_dns2: $(TEST_DIR)/test_dhcp_dns2.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_dhcp_dns2.c

$(TEST_BIN)/test_arp_ttl: $(TEST_DIR)/test_arp_ttl.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_arp_ttl.c

$(TEST_BIN)/test_pmm: $(TEST_DIR)/test_pmm.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_pmm.c

$(TEST_BIN)/test_bootmain_walk: $(TEST_DIR)/test_bootmain_walk.c | $(TEST_BIN)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_DIR)/test_bootmain_walk.c

unittest: $(TEST_BIN)/test_sha256 $(TEST_BIN)/test_inet_addr $(TEST_BIN)/test_mixer $(TEST_BIN)/test_ipv4_checksum $(TEST_BIN)/test_tcp_options $(TEST_BIN)/test_sched $(TEST_BIN)/test_tcp_state $(TEST_BIN)/test_ipv4_parse $(TEST_BIN)/test_arp_cache $(TEST_BIN)/test_mbr_parse $(TEST_BIN)/test_dhcp_options $(TEST_BIN)/test_eth_frame $(TEST_BIN)/test_arp_storm $(TEST_BIN)/test_stinkfs_dir $(TEST_BIN)/test_tcp_dupack $(TEST_BIN)/test_tcp_sack_use $(TEST_BIN)/test_icmp_ratelimit $(TEST_BIN)/test_tcp_persist $(TEST_BIN)/test_tcp_timewait $(TEST_BIN)/test_dns_retry $(TEST_BIN)/test_dhcp_retry $(TEST_BIN)/test_blit_overflow $(TEST_BIN)/test_udp_checksum $(TEST_BIN)/test_tcp_checksum $(TEST_BIN)/test_tcp_rst_gate $(TEST_BIN)/test_tcp_rxwnd $(TEST_BIN)/test_tcp_syn_gate $(TEST_BIN)/test_ipv4_unicast $(TEST_BIN)/test_ipv4_srcroute $(TEST_BIN)/test_rtc_alarm $(TEST_BIN)/test_tcp_tcb_sim $(TEST_BIN)/test_audio_mode $(TEST_BIN)/test_mbr_write $(TEST_BIN)/test_tcp_close_pid $(TEST_BIN)/test_icmp_echo $(TEST_BIN)/test_arp_ratelimit $(TEST_BIN)/test_utf8_collapse $(TEST_BIN)/test_dhcp_dns2 $(TEST_BIN)/test_arp_ttl $(TEST_BIN)/test_pmm $(TEST_BIN)/test_ipv4_martian $(TEST_BIN)/test_ipv4_teardrop $(TEST_BIN)/test_fb_rect_clip $(TEST_BIN)/test_mmap_overflow $(TEST_BIN)/test_blit_scale $(TEST_BIN)/test_tcp_keepalive $(TEST_BIN)/test_dns_cache $(TEST_BIN)/test_tcp_cwnd $(TEST_BIN)/test_tcp_wscale $(TEST_BIN)/test_tcp_ooo $(TEST_BIN)/test_ipv4_reasm_alloc $(TEST_BIN)/test_pipe $(TEST_BIN)/test_klog $(TEST_BIN)/test_timer $(TEST_BIN)/test_tcp_rto $(TEST_BIN)/test_fs_grow $(TEST_BIN)/test_fs_delete $(TEST_BIN)/test_elf_loader $(TEST_BIN)/test_keymap_br $(TEST_BIN)/test_vfs_fd $(TEST_BIN)/test_paging_brk $(TEST_BIN)/test_proc_reap $(TEST_BIN)/test_mouse_packet $(TEST_BIN)/test_bootmain_walk
	@echo "=== unit tests ==="
	$(TEST_BIN)/test_sha256
	$(TEST_BIN)/test_inet_addr
	$(TEST_BIN)/test_mixer
	$(TEST_BIN)/test_ipv4_checksum
	$(TEST_BIN)/test_tcp_options
	$(TEST_BIN)/test_sched
	$(TEST_BIN)/test_tcp_state
	$(TEST_BIN)/test_ipv4_parse
	$(TEST_BIN)/test_arp_cache
	$(TEST_BIN)/test_mbr_parse
	$(TEST_BIN)/test_dhcp_options
	$(TEST_BIN)/test_eth_frame
	$(TEST_BIN)/test_arp_storm
	$(TEST_BIN)/test_stinkfs_dir
	$(TEST_BIN)/test_tcp_dupack
	$(TEST_BIN)/test_tcp_sack_use
	$(TEST_BIN)/test_icmp_ratelimit
	$(TEST_BIN)/test_tcp_persist
	$(TEST_BIN)/test_tcp_timewait
	$(TEST_BIN)/test_dns_retry
	$(TEST_BIN)/test_dhcp_retry
	$(TEST_BIN)/test_blit_overflow
	$(TEST_BIN)/test_udp_checksum
	$(TEST_BIN)/test_tcp_checksum
	$(TEST_BIN)/test_tcp_rst_gate
	$(TEST_BIN)/test_tcp_rxwnd
	$(TEST_BIN)/test_tcp_syn_gate
	$(TEST_BIN)/test_ipv4_unicast
	$(TEST_BIN)/test_ipv4_srcroute
	$(TEST_BIN)/test_rtc_alarm
	$(TEST_BIN)/test_tcp_tcb_sim
	$(TEST_BIN)/test_audio_mode
	$(TEST_BIN)/test_mbr_write
	$(TEST_BIN)/test_tcp_close_pid
	$(TEST_BIN)/test_icmp_echo
	$(TEST_BIN)/test_arp_ratelimit
	$(TEST_BIN)/test_utf8_collapse
	$(TEST_BIN)/test_dhcp_dns2
	$(TEST_BIN)/test_arp_ttl
	$(TEST_BIN)/test_pmm
	$(TEST_BIN)/test_ipv4_martian
	$(TEST_BIN)/test_ipv4_teardrop
	$(TEST_BIN)/test_fb_rect_clip
	$(TEST_BIN)/test_mmap_overflow
	$(TEST_BIN)/test_blit_scale
	$(TEST_BIN)/test_tcp_keepalive
	$(TEST_BIN)/test_dns_cache
	$(TEST_BIN)/test_tcp_cwnd
	$(TEST_BIN)/test_tcp_wscale
	$(TEST_BIN)/test_tcp_ooo
	$(TEST_BIN)/test_ipv4_reasm_alloc
	$(TEST_BIN)/test_pipe
	$(TEST_BIN)/test_klog
	$(TEST_BIN)/test_timer
	$(TEST_BIN)/test_tcp_rto
	$(TEST_BIN)/test_fs_grow
	$(TEST_BIN)/test_fs_delete
	$(TEST_BIN)/test_elf_loader
	$(TEST_BIN)/test_keymap_br
	$(TEST_BIN)/test_vfs_fd
	$(TEST_BIN)/test_paging_brk
	$(TEST_BIN)/test_proc_reap
	$(TEST_BIN)/test_mouse_packet
	$(TEST_BIN)/test_bootmain_walk

clean:
	rm -rf $(BUILD) os.bin stinkos-install.iso

.PHONY: all hex dall readelf-kernel run run-install run-installed run-iso test-headless smoke-multiproc audit sample-packages unittest clean
