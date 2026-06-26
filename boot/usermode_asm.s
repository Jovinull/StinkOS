# Drop to ring 3 by building an inter-privilege iret frame and returning into it.
.code32

# void enter_user_mode(unsigned int entry, unsigned int user_stack)
.global enter_user_mode
enter_user_mode:
	mov 4(%esp), %ecx          # entry point (EIP)
	mov 8(%esp), %edx          # user stack top (ESP)

	mov $0x23, %ax             # user data selector (RPL 3)
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs

	push $0x23                 # SS
	push %edx                  # ESP
	pushf
	pop %eax
	or $0x200, %eax            # set IF so interrupts stay on in ring 3
	push %eax                  # EFLAGS
	push $0x1B                 # CS (user code selector, RPL 3)
	push %ecx                  # EIP
	iret

# Minimal setjmp/longjmp so the kernel can resume the menu after an app exits.
# struct kctx { esp, ebp, ebx, esi, edi, eip } at offsets 0,4,8,12,16,20.

# int ksetjmp(struct kctx *ctx)  -> 0 on the saving call, nonzero via klongjmp
.global ksetjmp
ksetjmp:
	mov 4(%esp), %eax
	mov (%esp), %ecx           # return address
	mov %ecx, 20(%eax)
	mov %esp, 0(%eax)
	mov %ebp, 4(%eax)
	mov %ebx, 8(%eax)
	mov %esi, 12(%eax)
	mov %edi, 16(%eax)
	xor %eax, %eax
	ret

# void klongjmp(struct kctx *ctx, int val)  -> returns val from ksetjmp's site
.global klongjmp
klongjmp:
	mov 4(%esp), %eax          # ctx
	mov 8(%esp), %edx          # val
	mov 0(%eax), %esp
	mov 4(%eax), %ebp
	mov 8(%eax), %ebx
	mov 12(%eax), %esi
	mov 16(%eax), %edi
	mov 20(%eax), %ecx
	mov %edx, %eax
	jmp *%ecx
