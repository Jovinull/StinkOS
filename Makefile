BUILD = build

CC = i386-elf-gcc
AS = i386-elf-as
LD = i386-elf-ld
CFLAGS = -O0 -m32 -ffreestanding -fno-pie -fno-stack-protector -Wall -Wextra \
         -ffunction-sections -fdata-sections -Ilib

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
                $(BUILD)/libstink_sha256.o

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

# Image is padded so the bootloader's fixed LBA read never runs past EOF.
# Must cover the boot sector + KSECTORS (see boot.s): (1 + 127) * 512 = 65536.
IMG_MIN = 65536
# Bytes the bootloader loads (boot sector + KSECTORS). The linked kernel image
# (boot + code + data, up to __bss_start) must fit here or it boots truncated.
KERNEL_LOAD_MAX = 65536

# All userland apps are stored as named ELF files inside StinkFS (no fixed-LBA
# slots). The StinkFS directory occupies LBA 128-129 (2 sectors); the data
# region starts at LBA 130 and extends ~100 MiB. App ELFs are written in order
# by make-stinkfs.py so that the menu's positional navigation matches the test.
FS_DIR_LBA  = 128
FS_DATA_LBA = 130
FS_DATA_END = 200130      # must match FS_DATA_END in fs.c (~100 MiB)
DISK_END    = 102466560   # FS_DATA_END * 512

# WAD bundling. Defaults look under wads/ for the three Freedoom releases the
# fetch-wads.sh script downloads; override on the command line to point at a
# specific WAD or to disable a slot. Missing files are skipped silently.
FREEDOOM1_WAD ?= wads/freedoom1.wad
FREEDOOM2_WAD ?= wads/freedoom2.wad
FREEDM_WAD    ?= wads/freedm.wad

C_SRCS  = main.c serial.c trap.c syscall.c bootdiag.c keyboard.c vbe.c fb.c font.c pmm.c paging.c gdt.c ata.c elf.c speaker.c fs.c vfs.c menu.c mouse.c rtc.c audio.c dma.c pci.c e1000.c net.c ethernet.c arp.c ipv4.c icmp.c udp.c dhcp.c dns.c tcp.c mbr.c
C_OBJS  = $(addprefix $(BUILD)/, $(C_SRCS:.c=.o))
# boot.o must link first (its _start sits at 0x7c00, pm_entry at 0x7e00).
LINK_OBJS = $(BUILD)/boot.o $(BUILD)/interrupts_asm.o $(BUILD)/gdt_asm.o $(BUILD)/usermode_asm.o $(C_OBJS)

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

os: $(LINK_OBJS) boot/linker.ld $(BUILD)/hello.elf $(BUILD)/box.elf $(BUILD)/fault.elf $(BUILD)/game.elf $(BUILD)/hi.elf $(BUILD)/anim.elf $(BUILD)/beep.elf $(BUILD)/save.elf $(BUILD)/files.elf $(BUILD)/ls.elf $(BUILD)/del.elf $(BUILD)/play.elf $(BUILD)/seek.elf $(BUILD)/fd.elf $(BUILD)/shell.elf $(BUILD)/arrows.elf $(BUILD)/snake.elf $(BUILD)/pong.elf $(BUILD)/installer.elf $(BUILD)/edit.elf $(BUILD)/fbdemo.elf $(BUILD)/stinkpkg.elf $(BUILD)/doom1.elf $(BUILD)/doom2.elf $(BUILD)/freedm.elf
	$(LD) -T boot/linker.ld --oformat binary -o os.bin $(LINK_OBJS)
	@size=$$(stat -c%s os.bin); if [ $$size -gt $(KERNEL_LOAD_MAX) ]; then \
		echo "ERROR: kernel image $$size B > bootloader load $(KERNEL_LOAD_MAX) B; raise KSECTORS in boot.s"; \
		exit 1; fi
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
	  INSTALLER.ELF=$(BUILD)/installer.elf \
	  EDIT.ELF=$(BUILD)/edit.elf \
	  STINKPKG.ELF=$(BUILD)/stinkpkg.elf \
	  DOOM1.ELF=$(BUILD)/doom1.elf \
	  DOOM2.ELF=$(BUILD)/doom2.elf \
	  FREEDM.ELF=$(BUILD)/freedm.elf \
	  FBDEMO.ELF=$(BUILD)/fbdemo.elf"; \
	  if [ -f "$(FREEDOOM1_WAD)" ]; then args="$$args FREEDOOM1.WAD=$(FREEDOOM1_WAD)"; fi; \
	  if [ -f "$(FREEDOOM2_WAD)" ]; then args="$$args FREEDOOM2.WAD=$(FREEDOOM2_WAD)"; fi; \
	  if [ -f "$(FREEDM_WAD)" ];    then args="$$args FREEDM.WAD=$(FREEDM_WAD)"; fi; \
	  python3 tools/make-stinkfs.py os.bin $(FS_DIR_LBA) $(FS_DATA_LBA) $(FS_DATA_END) $$args

hex:
	hexdump os.bin

dall:
	objdump -m i386 -b binary --adjust-vma=0x7c00 -D os.bin

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

clean:
	rm -rf $(BUILD) os.bin

.PHONY: all hex dall run run-install run-installed test-headless clean
