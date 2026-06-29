# C runtime startup for StinkOS userland apps. Linked first so _start
# sits at the load address (0x400000); calls main() then exits via
# SYS_EXIT. Pre-v0.9 the syscall ignored its ebx; v0.9 reads ebx as the
# exit code, defaulting here to 0 so existing void-main apps keep their
# old behavior. Apps that want a non-zero code call sys_exit_code(N).
.code32
.global _start
_start:
	call main
	xor %ebx, %ebx         # exit code 0 by default (override via sys_exit_code)
	mov $5, %eax           # SYS_EXIT
	int $0x80
hang:
	jmp hang
