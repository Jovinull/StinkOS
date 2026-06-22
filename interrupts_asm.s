# Low-level interrupt stubs (32-bit protected mode).
# Each stub pushes a (dummy) error code and the vector number, then jumps to a
# common path that saves state and calls into C.

.code32

# void idt_load(unsigned int idt_ptr_addr)
.global idt_load
idt_load:
	mov 4(%esp), %eax
	lidt (%eax)
	ret

# CPU exception stub without a hardware error code: push a dummy 0.
.macro ISR_NOERR num
.global isr\num
isr\num:
	cli
	push $0
	push $\num
	jmp isr_common
.endm

# CPU exception stub with a hardware error code already on the stack.
.macro ISR_ERR num
.global isr\num
isr\num:
	cli
	push $\num
	jmp isr_common
.endm

# Hardware IRQ stub: pushes a dummy error code and the remapped vector.
.macro IRQ num, vec
.global irq\num
irq\num:
	cli
	push $0
	push $\vec
	jmp irq_common
.endm

	ISR_NOERR 0
	ISR_NOERR 1
	ISR_NOERR 2
	ISR_NOERR 3
	ISR_NOERR 4
	ISR_NOERR 5
	ISR_NOERR 6
	ISR_NOERR 7
	ISR_ERR   8
	ISR_NOERR 9
	ISR_ERR   10
	ISR_ERR   11
	ISR_ERR   12
	ISR_ERR   13
	ISR_ERR   14
	ISR_NOERR 15
	ISR_NOERR 16
	ISR_ERR   17
	ISR_NOERR 18
	ISR_NOERR 19
	ISR_NOERR 20
	ISR_NOERR 21
	ISR_NOERR 22
	ISR_NOERR 23
	ISR_NOERR 24
	ISR_NOERR 25
	ISR_NOERR 26
	ISR_NOERR 27
	ISR_NOERR 28
	ISR_NOERR 29
	ISR_NOERR 30
	ISR_NOERR 31

	IRQ 0,  32
	IRQ 1,  33
	IRQ 2,  34
	IRQ 3,  35
	IRQ 4,  36
	IRQ 5,  37
	IRQ 6,  38
	IRQ 7,  39
	IRQ 8,  40
	IRQ 9,  41
	IRQ 10, 42
	IRQ 11, 43
	IRQ 12, 44
	IRQ 13, 45
	IRQ 14, 46
	IRQ 15, 47

.extern isr_handler
.extern irq_handler

# Save full state, switch to kernel data segment, call the C handler, restore.
isr_common:
	pusha
	xor %eax, %eax
	mov %ds, %ax
	push %eax                  # save data segment selector
	mov $0x10, %ax
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs
	push %esp                  # struct regs *
	call isr_handler
	add $4, %esp
	pop %eax                   # restore data segment selector
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs
	popa
	add $8, %esp               # drop int_no and err_code
	iret

irq_common:
	pusha
	xor %eax, %eax
	mov %ds, %ax
	push %eax
	mov $0x10, %ax
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs
	push %esp
	call irq_handler
	add $4, %esp
	pop %eax
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs
	popa
	add $8, %esp
	iret
