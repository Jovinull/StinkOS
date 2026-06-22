# StinkOS userland app "FAULT": logs a line, then deliberately reads kernel
# memory (a supervisor page) so the CPU raises a fault in ring 3. The kernel
# should kill this app and return to the menu instead of crashing.
.code32
.global _start
_start:
	mov $1, %eax           # SYS_LOG
	mov $msg, %ebx
	int $0x80

	mov 0x100000, %eax     # touch kernel memory -> page fault (ring 3)
hang:
	jmp hang

msg:
	.asciz "fault app running"
