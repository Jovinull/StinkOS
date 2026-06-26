# Load a new GDT and reload the segment registers / task register.
.code32

# void gdt_flush(unsigned int gdt_ptr_addr)
.global gdt_flush
gdt_flush:
	mov 4(%esp), %eax
	lgdt (%eax)
	mov $0x10, %ax             # kernel data selector
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs
	mov %ax, %ss
	ljmp $0x08, $gdt_flush_done  # reload CS = kernel code selector
gdt_flush_done:
	ret

# void tss_flush(void)
.global tss_flush
tss_flush:
	mov $0x28, %ax             # TSS selector (GDT index 5)
	ltr %ax
	ret
