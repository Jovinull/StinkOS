BUILD = build

all: os

$(BUILD):
	mkdir -p $(BUILD)

boot: $(BUILD)
	i386-elf-as -O0 boot.s -o $(BUILD)/boot.o

kernel: $(BUILD)
	i386-elf-gcc -O0 -m16 -ffreestanding -c -o $(BUILD)/kernel.o kernel.c -Ttext 0x7e00

os: boot kernel
	i386-elf-ld $(BUILD)/boot.o $(BUILD)/kernel.o -o os.bin --oformat binary -Ttext 0x7c00

hex:
	hexdump os.bin

dkernel:
	objdump -D $(BUILD)/kernel.o

dboot:
	objdump -D $(BUILD)/boot.o

dall:
	objdump -m i386 -b binary --adjust-vma=0x7c00 -D os.bin

run: all
	qemu-system-i386 os.bin

clean:
	rm -rf $(BUILD) os.bin

.PHONY: all boot kernel os hex dkernel dboot dall run clean
