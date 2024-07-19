all:
	i386-elf-as -O0 boot.s -o ./Boot/boot.o
	i386-elf-gcc -O0 -m16 -c -o ./Kernel/kernel.o kernel.c -Ttext 0x7e00

	i386-elf-ld ./Boot/boot.o ./Kernel/kernel.o -o os.bin --oformat binary -Ttext 0x7c00

boot:
	i386-elf-as -O0 boot.s -o ./Boot/boot.o

kernel:
	i386-elf-gcc -O0 -m16 -c -o ./Kernel/kernel.o kernel.c -Ttext 0x7e00

os:
	i386-elf-ld ./Boot/boot.o ./Kernel/kernel.o -o os.bin --oformat binary -Ttext 0x7c00

hex:
	hexdump os.bin

dkernel:
	objdump -D ./Kernel/kernel.o

dboot:
	objdump -D ./Boot/boot.o

dall:
	objdump -m i386 -b binary --adjust-vma=0x7c00 -D os.bin

run:
	qemu-system-i386 os.bin
