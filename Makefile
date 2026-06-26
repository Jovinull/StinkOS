BUILD = build

CC = i386-elf-gcc
AS = i386-elf-as
LD = i386-elf-ld
CFLAGS = -O0 -m32 -ffreestanding -fno-pie -fno-stack-protector -Wall -Wextra \
         -ffunction-sections -fdata-sections
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
                $(BUILD)/libstink_setjmp.o

# Image is padded so the bootloader's fixed LBA read never runs past EOF.
# Must cover the boot sector + KSECTORS (see boot.s): (1 + 56) * 512 = 29184.
IMG_MIN = 29184
# Bytes the bootloader loads (boot sector + KSECTORS). The linked kernel image
# (boot + code + data, up to __bss_start) must fit here or it boots truncated.
KERNEL_LOAD_MAX = 29184

# Userland apps stored on raw disk slots, loaded by the kernel at runtime.
APP1_LBA = 64
APP2_LBA = 72
APP3_LBA = 80
APP4_LBA = 88
APP5_LBA = 96
APP6_LBA = 104
APP7_LBA = 112
APP8_LBA = 120
APP9_LBA = 128
APP10_LBA = 136
APP11_LBA = 144
APP12_LBA = 152
APP13_LBA = 160
APP14_LBA = 168
APP15_LBA = 176           # SHELL  — 16-sector slot (176..191)
APP16_LBA = 192           # ARROWS — 8-sector slot  (192..199)
APP17_LBA = 200           # SNAKE  — 8-sector slot  (200..207)
APP18_LBA = 208           # PONG   — 8-sector slot  (208..215)

# The app region spans LBA 64..223. Metadata lives above it: the app TOC at
# LBA 224, then StinkFS (directory at 225, 32-sector data region at 226..257).
TOC_LBA  = 224
DISK_END = 132096         # 258 * 512: keep TOC and StinkFS present

C_SRCS  = kernel.c serial.c interrupts.c keyboard.c vbe.c fb.c font.c pmm.c paging.c gdt.c ata.c elf.c speaker.c fs.c vfs.c menu.c mouse.c rtc.c
C_OBJS  = $(addprefix $(BUILD)/, $(C_SRCS:.c=.o))
# boot.o must link first (its _start sits at 0x7c00, pm_entry at 0x7e00).
LINK_OBJS = $(BUILD)/boot.o $(BUILD)/interrupts_asm.o $(BUILD)/gdt_asm.o $(BUILD)/usermode_asm.o $(C_OBJS)

all: os

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.s | $(BUILD)
	$(AS) -O0 $< -o $@

# Userland support library: compiled once, linked into every C app.
$(BUILD)/libstink_alloc.o: apps/libstink_alloc.c apps/libstink.h | $(BUILD)
	$(CC) $(CFLAGS) -c apps/libstink_alloc.c -o $(BUILD)/libstink_alloc.o

$(BUILD)/libstink_printf.o: apps/libstink_printf.c apps/libstink.h | $(BUILD)
	$(CC) $(CFLAGS) -c apps/libstink_printf.c -o $(BUILD)/libstink_printf.o

$(BUILD)/libstink_stdio.o: apps/libstink_stdio.c apps/libstink.h | $(BUILD)
	$(CC) $(CFLAGS) -c apps/libstink_stdio.c -o $(BUILD)/libstink_stdio.o

# POSIX glue needs the doom-shims headers on its include path so its <time.h>
# and <sys/stat.h> includes find the layout it implements against.
$(BUILD)/libstink_posix.o: apps/libstink_posix.c apps/libstink.h apps/doom-shims/time.h apps/doom-shims/sys/stat.h | $(BUILD)
	$(CC) $(CFLAGS) -I apps/doom-shims -c apps/libstink_posix.c -o $(BUILD)/libstink_posix.o

$(BUILD)/libstink_setjmp.o: apps/libstink_setjmp.s | $(BUILD)
	$(AS) -O0 apps/libstink_setjmp.s -o $(BUILD)/libstink_setjmp.o

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
$(BUILD)/hi.elf: apps/crt0.s apps/hi.c apps/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/hi.c -o $(BUILD)/hi_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/hi.elf $(BUILD)/crt0.o $(BUILD)/hi_app.o $(LIBSTINK_OBJS)

$(BUILD)/anim.elf: apps/crt0.s apps/anim.c apps/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/anim.c -o $(BUILD)/anim_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/anim.elf $(BUILD)/crt0.o $(BUILD)/anim_app.o $(LIBSTINK_OBJS)

$(BUILD)/beep.elf: apps/crt0.s apps/beep.c apps/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/beep.c -o $(BUILD)/beep_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/beep.elf $(BUILD)/crt0.o $(BUILD)/beep_app.o $(LIBSTINK_OBJS)

$(BUILD)/save.elf: apps/crt0.s apps/save.c apps/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/save.c -o $(BUILD)/save_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/save.elf $(BUILD)/crt0.o $(BUILD)/save_app.o $(LIBSTINK_OBJS)

$(BUILD)/files.elf: apps/crt0.s apps/files.c apps/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/files.c -o $(BUILD)/files_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/files.elf $(BUILD)/crt0.o $(BUILD)/files_app.o $(LIBSTINK_OBJS)

$(BUILD)/ls.elf: apps/crt0.s apps/ls.c apps/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/ls.c -o $(BUILD)/ls_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/ls.elf $(BUILD)/crt0.o $(BUILD)/ls_app.o $(LIBSTINK_OBJS)

$(BUILD)/del.elf: apps/crt0.s apps/del.c apps/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/del.c -o $(BUILD)/del_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/del.elf $(BUILD)/crt0.o $(BUILD)/del_app.o $(LIBSTINK_OBJS)

$(BUILD)/play.elf: apps/crt0.s apps/play.c apps/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/play.c -o $(BUILD)/play_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/play.elf $(BUILD)/crt0.o $(BUILD)/play_app.o $(LIBSTINK_OBJS)

$(BUILD)/seek.elf: apps/crt0.s apps/seek.c apps/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/seek.c -o $(BUILD)/seek_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/seek.elf $(BUILD)/crt0.o $(BUILD)/seek_app.o $(LIBSTINK_OBJS)

$(BUILD)/fd.elf: apps/crt0.s apps/fd.c apps/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/fd.c -o $(BUILD)/fd_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/fd.elf $(BUILD)/crt0.o $(BUILD)/fd_app.o $(LIBSTINK_OBJS)

$(BUILD)/shell.elf: apps/crt0.s apps/shell.c apps/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/shell.c -o $(BUILD)/shell_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/shell.elf $(BUILD)/crt0.o $(BUILD)/shell_app.o $(LIBSTINK_OBJS)

$(BUILD)/arrows.elf: apps/crt0.s apps/arrows.c apps/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/arrows.c -o $(BUILD)/arrows_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/arrows.elf $(BUILD)/crt0.o $(BUILD)/arrows_app.o $(LIBSTINK_OBJS)

$(BUILD)/snake.elf: apps/crt0.s apps/snake.c apps/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/snake.c -o $(BUILD)/snake_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/snake.elf $(BUILD)/crt0.o $(BUILD)/snake_app.o $(LIBSTINK_OBJS)

$(BUILD)/pong.elf: apps/crt0.s apps/pong.c apps/libstink.h apps/app.ld $(LIBSTINK_OBJS) | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/pong.c -o $(BUILD)/pong_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/pong.elf $(BUILD)/crt0.o $(BUILD)/pong_app.o $(LIBSTINK_OBJS)

os: $(LINK_OBJS) linker.ld $(BUILD)/hello.elf $(BUILD)/box.elf $(BUILD)/fault.elf $(BUILD)/game.elf $(BUILD)/hi.elf $(BUILD)/anim.elf $(BUILD)/beep.elf $(BUILD)/save.elf $(BUILD)/files.elf $(BUILD)/ls.elf $(BUILD)/del.elf $(BUILD)/play.elf $(BUILD)/seek.elf $(BUILD)/fd.elf $(BUILD)/shell.elf $(BUILD)/arrows.elf $(BUILD)/snake.elf $(BUILD)/pong.elf
	$(LD) -T linker.ld --oformat binary -o os.bin $(LINK_OBJS)
	@size=$$(stat -c%s os.bin); if [ $$size -gt $(KERNEL_LOAD_MAX) ]; then \
		echo "ERROR: kernel image $$size B > bootloader load $(KERNEL_LOAD_MAX) B; raise KSECTORS in boot.s"; \
		exit 1; fi
	@size=$$(stat -c%s os.bin); if [ $$size -lt $(IMG_MIN) ]; then truncate -s $(IMG_MIN) os.bin; fi
	dd if=$(BUILD)/hello.elf of=os.bin bs=512 seek=$(APP1_LBA) conv=notrunc status=none
	dd if=$(BUILD)/box.elf   of=os.bin bs=512 seek=$(APP2_LBA) conv=notrunc status=none
	dd if=$(BUILD)/fault.elf of=os.bin bs=512 seek=$(APP3_LBA) conv=notrunc status=none
	dd if=$(BUILD)/game.elf  of=os.bin bs=512 seek=$(APP4_LBA) conv=notrunc status=none
	dd if=$(BUILD)/hi.elf    of=os.bin bs=512 seek=$(APP5_LBA) conv=notrunc status=none
	dd if=$(BUILD)/anim.elf  of=os.bin bs=512 seek=$(APP6_LBA) conv=notrunc status=none
	dd if=$(BUILD)/beep.elf  of=os.bin bs=512 seek=$(APP7_LBA) conv=notrunc status=none
	dd if=$(BUILD)/save.elf  of=os.bin bs=512 seek=$(APP8_LBA) conv=notrunc status=none
	dd if=$(BUILD)/files.elf of=os.bin bs=512 seek=$(APP9_LBA) conv=notrunc status=none
	dd if=$(BUILD)/ls.elf    of=os.bin bs=512 seek=$(APP10_LBA) conv=notrunc status=none
	dd if=$(BUILD)/del.elf   of=os.bin bs=512 seek=$(APP11_LBA) conv=notrunc status=none
	dd if=$(BUILD)/play.elf  of=os.bin bs=512 seek=$(APP12_LBA) conv=notrunc status=none
	dd if=$(BUILD)/seek.elf  of=os.bin bs=512 seek=$(APP13_LBA) conv=notrunc status=none
	dd if=$(BUILD)/fd.elf    of=os.bin bs=512 seek=$(APP14_LBA) conv=notrunc status=none
	dd if=$(BUILD)/shell.elf  of=os.bin bs=512 seek=$(APP15_LBA) conv=notrunc status=none
	dd if=$(BUILD)/arrows.elf of=os.bin bs=512 seek=$(APP16_LBA) conv=notrunc status=none
	dd if=$(BUILD)/snake.elf  of=os.bin bs=512 seek=$(APP17_LBA) conv=notrunc status=none
	dd if=$(BUILD)/pong.elf   of=os.bin bs=512 seek=$(APP18_LBA) conv=notrunc status=none
	python3 tools/make-toc.py $(BUILD)/toc.bin \
		"1 HELLO:$(APP1_LBA):$(BUILD)/hello.elf" \
		"2 BOX:$(APP2_LBA):$(BUILD)/box.elf" \
		"3 FAULT:$(APP3_LBA):$(BUILD)/fault.elf" \
		"4 GAME:$(APP4_LBA):$(BUILD)/game.elf" \
		"5 HIC:$(APP5_LBA):$(BUILD)/hi.elf" \
		"6 ANIM:$(APP6_LBA):$(BUILD)/anim.elf" \
		"7 BEEP:$(APP7_LBA):$(BUILD)/beep.elf" \
		"8 SAVE:$(APP8_LBA):$(BUILD)/save.elf" \
		"9 FILES:$(APP9_LBA):$(BUILD)/files.elf" \
		"10 LS:$(APP10_LBA):$(BUILD)/ls.elf" \
		"11 DEL:$(APP11_LBA):$(BUILD)/del.elf" \
		"12 PLAY:$(APP12_LBA):$(BUILD)/play.elf" \
		"13 SEEK:$(APP13_LBA):$(BUILD)/seek.elf" \
		"14 FD:$(APP14_LBA):$(BUILD)/fd.elf" \
		"15 SHELL:$(APP15_LBA):$(BUILD)/shell.elf" \
		"16 ARROWS:$(APP16_LBA):$(BUILD)/arrows.elf" \
		"17 SNAKE:$(APP17_LBA):$(BUILD)/snake.elf" \
		"18 PONG:$(APP18_LBA):$(BUILD)/pong.elf"
	dd if=$(BUILD)/toc.bin   of=os.bin bs=512 seek=$(TOC_LBA) conv=notrunc status=none
	@size=$$(stat -c%s os.bin); if [ $$size -lt $(DISK_END) ]; then truncate -s $(DISK_END) os.bin; fi

hex:
	hexdump os.bin

dall:
	objdump -m i386 -b binary --adjust-vma=0x7c00 -D os.bin

run: all
	qemu-system-i386 -drive format=raw,file=os.bin

# Headless verification: boots the image in qemu, reads the serial debug log and
# injects keystrokes via the monitor to assert protected mode, the timer IRQ and
# the keyboard IRQ all work. See tools/test-headless.py.
test-headless: all
	@python3 tools/test-headless.py

clean:
	rm -rf $(BUILD) os.bin

.PHONY: all hex dall run test-headless clean
