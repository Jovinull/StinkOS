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
