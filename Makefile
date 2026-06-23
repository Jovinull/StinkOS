BUILD = build

CC = i386-elf-gcc
AS = i386-elf-as
LD = i386-elf-ld
CFLAGS = -O0 -m32 -ffreestanding -fno-pie -fno-stack-protector -Wall -Wextra
# App link flags: omagic (-N) packs the loadable segment with no page-alignment
# gap, and -s strips the symbol tables, keeping each app ELF down to one sector.
# Apps are a single flat code+data region (one set of user pages), so the load
# segment is intentionally RWX; silence ld's advisory warning about that.
APP_LDFLAGS = -T apps/app.ld -N -s --no-warn-rwx-segments

# Image is padded so the bootloader's fixed LBA read never runs past EOF.
# Must cover the boot sector + KSECTORS (see boot.s): (1 + 40) * 512 = 20992.
IMG_MIN = 20992

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

# The app region spans LBA 64..191 (sixteen 8-sector slots). Metadata lives
# above it: app TOC, single-value save sector, then StinkFS (a directory sector
# at 194 and a 32-sector data region at 195..226).
TOC_LBA  = 192
SAVE_LBA = 193
DISK_END = 116224         # (195 + 32) * 512: keep TOC, save and StinkFS present

C_SRCS  = kernel.c serial.c interrupts.c keyboard.c vbe.c fb.c font.c pmm.c paging.c gdt.c ata.c elf.c speaker.c fs.c menu.c
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

# C userland apps: crt0 (entry) linked first, then the compiled C object.
$(BUILD)/hi.elf: apps/crt0.s apps/hi.c apps/libstink.h apps/app.ld | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/hi.c -o $(BUILD)/hi_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/hi.elf $(BUILD)/crt0.o $(BUILD)/hi_app.o

$(BUILD)/anim.elf: apps/crt0.s apps/anim.c apps/libstink.h apps/app.ld | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/anim.c -o $(BUILD)/anim_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/anim.elf $(BUILD)/crt0.o $(BUILD)/anim_app.o

$(BUILD)/beep.elf: apps/crt0.s apps/beep.c apps/libstink.h apps/app.ld | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/beep.c -o $(BUILD)/beep_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/beep.elf $(BUILD)/crt0.o $(BUILD)/beep_app.o

$(BUILD)/save.elf: apps/crt0.s apps/save.c apps/libstink.h apps/app.ld | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/save.c -o $(BUILD)/save_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/save.elf $(BUILD)/crt0.o $(BUILD)/save_app.o

$(BUILD)/files.elf: apps/crt0.s apps/files.c apps/libstink.h apps/app.ld | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/files.c -o $(BUILD)/files_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/files.elf $(BUILD)/crt0.o $(BUILD)/files_app.o

$(BUILD)/ls.elf: apps/crt0.s apps/ls.c apps/libstink.h apps/app.ld | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/ls.c -o $(BUILD)/ls_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/ls.elf $(BUILD)/crt0.o $(BUILD)/ls_app.o

$(BUILD)/del.elf: apps/crt0.s apps/del.c apps/libstink.h apps/app.ld | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/del.c -o $(BUILD)/del_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/del.elf $(BUILD)/crt0.o $(BUILD)/del_app.o

$(BUILD)/play.elf: apps/crt0.s apps/play.c apps/libstink.h apps/app.ld | $(BUILD)
	$(AS) -O0 apps/crt0.s -o $(BUILD)/crt0.o
	$(CC) $(CFLAGS) -c apps/play.c -o $(BUILD)/play_app.o
	$(LD) $(APP_LDFLAGS) -o $(BUILD)/play.elf $(BUILD)/crt0.o $(BUILD)/play_app.o

os: $(LINK_OBJS) linker.ld $(BUILD)/hello.elf $(BUILD)/box.elf $(BUILD)/fault.elf $(BUILD)/game.elf $(BUILD)/hi.elf $(BUILD)/anim.elf $(BUILD)/beep.elf $(BUILD)/save.elf $(BUILD)/files.elf $(BUILD)/ls.elf $(BUILD)/del.elf $(BUILD)/play.elf
	$(LD) -T linker.ld --oformat binary -o os.bin $(LINK_OBJS)
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
		"12 PLAY:$(APP12_LBA):$(BUILD)/play.elf"
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
