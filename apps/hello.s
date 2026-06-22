# StinkOS userland app (ring 3). Logs a line, then draws a 20x20 white square
# on screen -- both through int 0x80 syscalls. Flat binary linked at 0x400000,
# stored on a raw disk slot and loaded by the kernel.
#   syscall: eax = number, args in ebx/ecx/edx
#   1 = log     (ebx = string)
#   2 = draw    (ebx = x, ecx = y, edx = rgb)
#   3 = getkey  (-> eax = char, or 0 if none)
.code32
.global _start
_start:
	mov $1, %eax           # SYS_LOG
	mov $msg, %ebx
	int $0x80

	mov $10, %esi          # y = 10
yloop:
	mov $10, %edi          # x = 10
xloop:
	mov $2, %eax           # SYS_DRAW
	mov %edi, %ebx
	mov %esi, %ecx
	mov $0xFFFFFF, %edx    # white
	int $0x80
	inc %edi
	cmp $30, %edi          # x in [10, 30)
	jl xloop
	inc %esi
	cmp $30, %esi          # y in [10, 30)
	jl yloop

poll:                          # wait for a keypress from the kernel
	mov $3, %eax           # SYS_GETKEY
	int $0x80
	test %eax, %eax
	jz poll

	mov $1, %eax           # SYS_LOG: report we received input
	mov $msg2, %ebx
	int $0x80

hang:
	jmp hang

msg:
	.asciz "hello from disk app"
msg2:
	.asciz "app: key received"
