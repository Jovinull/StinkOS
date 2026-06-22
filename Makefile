BUILD = build

CC = i386-elf-gcc
AS = i386-elf-as
LD = i386-elf-ld
CFLAGS = -O0 -m32 -ffreestanding -fno-pie -fno-stack-protector -Wall -Wextra

# Image is padded so the bootloader's fixed LBA read never runs past EOF.
# Must cover the boot sector + KSECTORS (see boot.s): (1 + 40) * 512 = 20992.
IMG_MIN = 20992

C_SRCS  = kernel.c serial.c interrupts.c keyboard.c
C_OBJS  = $(addprefix $(BUILD)/, $(C_SRCS:.c=.o))
# boot.o must link first (its _start sits at 0x7c00, pm_entry at 0x7e00).
LINK_OBJS = $(BUILD)/boot.o $(BUILD)/interrupts_asm.o $(C_OBJS)

all: os

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.s | $(BUILD)
	$(AS) -O0 $< -o $@

os: $(LINK_OBJS) linker.ld
	$(LD) -T linker.ld --oformat binary -o os.bin $(LINK_OBJS)
	@size=$$(stat -c%s os.bin); if [ $$size -lt $(IMG_MIN) ]; then truncate -s $(IMG_MIN) os.bin; fi

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
