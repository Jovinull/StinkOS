# StinkOS userland game: move a 16x16 block with w/a/s/d, quit with q.
# Pure ring-3 program using the syscalls:
#   2 = draw  (ebx=x, ecx=y, edx=rgb)
#   3 = getkey (-> eax)
#   5 = exit
.code32
.global _start

.set STEP, 16
.set SIZE, 16

_start:
	movl $0x00FF00, block_color      # green
	call draw_block

game_loop:
	mov $3, %eax                     # SYS_GETKEY
	int $0x80
	test %eax, %eax
	jz game_loop
	mov %eax, %ebp                   # keep key in ebp (preserved across syscalls)

	movl $0x001022, block_color      # erase old block (background colour)
	call draw_block

	cmp $0x71, %ebp                  # 'q' -> quit
	je quit
	cmp $0x77, %ebp                  # 'w' -> up
	je go_up
	cmp $0x73, %ebp                  # 's' -> down
	je go_down
	cmp $0x61, %ebp                  # 'a' -> left
	je go_left
	cmp $0x64, %ebp                  # 'd' -> right
	je go_right
	jmp redraw

go_up:    subl $STEP, cur_y ; jmp redraw
go_down:  addl $STEP, cur_y ; jmp redraw
go_left:  subl $STEP, cur_x ; jmp redraw
go_right: addl $STEP, cur_x ; jmp redraw

redraw:
	movl $0x00FF00, block_color
	call draw_block
	jmp game_loop

quit:
	mov $5, %eax                     # SYS_EXIT
	int $0x80
hang:
	jmp hang

# draw a SIZE x SIZE square at (cur_x, cur_y) in block_color.
# esi = y, edi = x; both survive int 0x80 (kernel preserves them via pusha/popa).
draw_block:
	mov cur_y, %esi
db_row:
	mov cur_x, %edi
db_col:
	mov $2, %eax                     # SYS_DRAW
	mov %edi, %ebx
	mov %esi, %ecx
	mov block_color, %edx
	int $0x80
	inc %edi
	mov cur_x, %eax
	add $SIZE, %eax
	cmp %eax, %edi
	jl db_col
	inc %esi
	mov cur_y, %eax
	add $SIZE, %eax
	cmp %eax, %esi
	jl db_row
	ret

cur_x:       .long 100
cur_y:       .long 100
block_color: .long 0x00FF00
