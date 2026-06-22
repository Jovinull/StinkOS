# StinkOS userland app "BOX": logs a line and draws a red square, via syscalls.
# Flat binary linked at 0x400000, stored on a raw disk slot, launched from the menu.
#   1 = log    (ebx = string)
#   2 = draw   (ebx = x, ecx = y, edx = rgb)
#   3 = getkey (-> eax = char, or 0)
#   5 = exit   (return to the menu)
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

poll:                          # stay on screen until a key is pressed
	mov $3, %eax           # SYS_GETKEY
	int $0x80
	test %eax, %eax
	jz poll

	mov $5, %eax           # SYS_EXIT: back to the menu
	int $0x80
hang:
	jmp hang

msg:
	.asciz "box app running"
