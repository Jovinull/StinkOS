.code16
.global _start
_start:

mov $0x00, %ah
mov $0x03, %al

int $0x10

mov %dl, disk

mov $0, %bx
mov %bx, %ss
mov %bx, %es
mov %bx, %ds

mov $0x0, %bp
mov %bp, %sp

mov $2, %ah
mov $2, %al
mov $0, %ch
mov $2, %cl
mov $0, %dh
mov (disk), %dl

push $0
pop %es

mov $0x7e00, %bx

int $0x13

push $'A'

.extern
call kernel

jmp .

disk: .byte 0
pos: .fill 2, 1, 10

.fill 510-(.-_start), 1, 0
.word 0xaa55

.global putchar
putchar:

push %bp
mov %sp, %bp

mov 6(%bp), %al
mov $0x0e, %ah
int $0x10

pop %bp
ret
