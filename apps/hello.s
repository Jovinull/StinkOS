# StinkOS userland app: runs in ring 3, asks the kernel to log a line via the
# int 0x80 syscall (eax = number, ebx = arg), then idles. Built as a flat binary
# linked at 0x400000, stored on a raw disk slot and loaded by the kernel.
.code32
.global _start
_start:
	mov $1, %eax           # SYS_LOG
	mov $msg, %ebx         # pointer to the string (absolute, linked at 0x400000)
	int $0x80
hang:
	jmp hang

msg:
	.asciz "hello from disk app"
