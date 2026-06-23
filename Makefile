BUILD = build

CC = i386-elf-gcc
AS = i386-elf-as
LD = i386-elf-ld
CFLAGS = -O0 -m32 -ffreestanding -fno-pie -fno-stack-protector -Wall -Wextra
# App link flags: omagic (-N) packs the loadable segment with no page-alignment
# gap, and -s strips the symbol tables, keeping each app ELF down to one sector.
APP_LDFLAGS = -T apps/app.ld -N -s

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

# App table-of-contents (mini-filesystem) sector and final image size.
TOC_LBA  = 120
TOC_END  = 61952          # (TOC_LBA + 1) * 512: keep the TOC sector present

C_SRCS  = kernel.c serial.c interrupts.c keyboard.c vbe.c fb.c font.c pmm.c paging.c gdt.c ata.c elf.c fs.c menu.c
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

os: $(LINK_OBJS) linker.ld $(BUILD)/hello.elf $(BUILD)/box.elf $(BUILD)/fault.elf $(BUILD)/game.elf $(BUILD)/hi.elf $(BUILD)/anim.elf
	$(LD) -T linker.ld --oformat binary -o os.bin $(LINK_OBJS)
	@size=$$(stat -c%s os.bin); if [ $$size -lt $(IMG_MIN) ]; then truncate -s $(IMG_MIN) os.bin; fi
	dd if=$(BUILD)/hello.elf of=os.bin bs=512 seek=$(APP1_LBA) conv=notrunc status=none
	dd if=$(BUILD)/box.elf   of=os.bin bs=512 seek=$(APP2_LBA) conv=notrunc status=none
	dd if=$(BUILD)/fault.elf of=os.bin bs=512 seek=$(APP3_LBA) conv=notrunc status=none
	dd if=$(BUILD)/game.elf  of=os.bin bs=512 seek=$(APP4_LBA) conv=notrunc status=none
	dd if=$(BUILD)/hi.elf    of=os.bin bs=512 seek=$(APP5_LBA) conv=notrunc status=none
	dd if=$(BUILD)/anim.elf  of=os.bin bs=512 seek=$(APP6_LBA) conv=notrunc status=none
	python3 tools/make-toc.py $(BUILD)/toc.bin \
		"1 HELLO:$(APP1_LBA):$(BUILD)/hello.elf" \
		"2 BOX:$(APP2_LBA):$(BUILD)/box.elf" \
		"3 FAULT:$(APP3_LBA):$(BUILD)/fault.elf" \
		"4 GAME:$(APP4_LBA):$(BUILD)/game.elf" \
		"5 HIC:$(APP5_LBA):$(BUILD)/hi.elf" \
		"6 ANIM:$(APP6_LBA):$(BUILD)/anim.elf"
	dd if=$(BUILD)/toc.bin   of=os.bin bs=512 seek=$(TOC_LBA) conv=notrunc status=none
	@size=$$(stat -c%s os.bin); if [ $$size -lt $(TOC_END) ]; then truncate -s $(TOC_END) os.bin; fi

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
