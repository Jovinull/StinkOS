BUILD = build

# Image is padded so the bootloader's fixed LBA read never runs past EOF.
# Must cover the boot sector + KSECTORS (see boot.s): (1 + 40) * 512 = 20992.
IMG_MIN = 20992

all: os

$(BUILD):
	mkdir -p $(BUILD)

boot: $(BUILD)
	i386-elf-as -O0 boot.s -o $(BUILD)/boot.o

kernel: $(BUILD)
	i386-elf-gcc -O0 -m32 -ffreestanding -fno-pie -fno-stack-protector -c kernel.c -o $(BUILD)/kernel.o

os: boot kernel
	i386-elf-ld -Ttext 0x7c00 --oformat binary -o os.bin $(BUILD)/boot.o $(BUILD)/kernel.o
	@size=$$(stat -c%s os.bin); if [ $$size -lt $(IMG_MIN) ]; then truncate -s $(IMG_MIN) os.bin; fi

hex:
	hexdump os.bin

dkernel:
	objdump -D $(BUILD)/kernel.o

dboot:
	objdump -D $(BUILD)/boot.o

dall:
	objdump -m i386 -b binary --adjust-vma=0x7c00 -D os.bin

run: all
	qemu-system-i386 -drive format=raw,file=os.bin

# Headless smoke test: boot, capture the kernel's serial debug log, and assert
# it reached protected mode. Greps for the exact boot string the kernel emits on
# COM1 -- a real check, not just "did not triple-fault".
EXPECT = StinkOS: protected mode active
test-headless: all
	@out=$$(timeout 3s qemu-system-i386 -drive format=raw,file=os.bin -display none -serial stdio < /dev/null 2>/dev/null); \
	if echo "$$out" | grep -qF "$(EXPECT)"; then \
		echo "PASS: kernel reached protected mode (serial: \"$(EXPECT)\")"; \
	else \
		echo "FAIL: expected serial output not found"; echo "--- serial output ---"; echo "$$out"; exit 1; \
	fi

clean:
	rm -rf $(BUILD) os.bin

.PHONY: all boot kernel os hex dkernel dboot dall run test-headless clean
