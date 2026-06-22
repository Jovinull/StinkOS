# StinkOS userland app "BOX": logs a line and draws a red square, via syscalls.
# Flat binary linked at 0x400000, stored on a raw disk slot, launched from the menu.
#   1 = log   (ebx = string)
#   2 = draw  (ebx = x, ecx = y, edx = rgb)
.code32
.global _start
_start:
	mov $1, %eax           # SYS_LOG
	mov $msg, %ebx
	int $0x80

	mov $40, %esi          # y = 40
yloop:
	mov $40, %edi          # x = 40
xloop:
	mov $2, %eax           # SYS_DRAW
	mov %edi, %ebx
	mov %esi, %ecx
	mov $0xFF0000, %edx    # red
	int $0x80
	inc %edi
	cmp $70, %edi          # x in [40, 70)
	jl xloop
	inc %esi
	cmp $70, %esi          # y in [40, 70)
	jl yloop

hang:
	jmp hang

msg:
	.asciz "box app running"
