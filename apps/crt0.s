# C runtime startup for StinkOS userland apps. Linked first so _start sits at
# the load address (0x400000); calls main() then exits via the SYS_EXIT syscall.
.code32
.global _start
_start:
	call main
	mov $5, %eax           # SYS_EXIT (in case main returns)
	int $0x80
hang:
	jmp hang
