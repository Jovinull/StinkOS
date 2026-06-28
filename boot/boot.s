# StinkOS bootblock (TODO §13: ELF-aware boot, see boot/bootmain.c).
#
# The bootblock is the BIOS-loaded sector 0 plus the post-510-byte tail
# that lives in sectors 1..BOOTBLOCK_SECTORS-1. Everything is linked at
# 0x7C00 by boot/bootblock.ld. The 16-bit prologue (this file's first
# half) reads the rest of the bootblock off disk via INT 13h, enables
# A20, loads a flat GDT, and jumps into 32-bit mode. pm_entry (this
# file's second half) zeroes the bootblock's .bss, sets up segment
# registers + stack, and calls bootmain() (in boot/bootmain.c).
# bootmain reads the kernel ELF starting at KERNEL_LBA, walks its
# PT_LOAD program headers, copies each segment to its linked physical
# address (kernel is linked at 0x100000 by boot/kernel.ld), and jumps
# to the kernel entry. From the moment bootmain returns control to
# the kernel, the bootblock memory (0x7C00..0x9FFF) is dead.

.equ BOOTBLOCK_SECTORS, 16   # bootblock = sector 0 + 15 follow-on sectors
                             # (boot.s + compiled bootmain.o). Plenty of
                             # headroom: bootmain.o is ~700 bytes and we
                             # cap at 8 KiB total here.
.equ LOAD_ADDR,  0x7E00      # where the post-sector-0 bootblock lands.
                             # BIOS already put us at 0x7C00, so the tail
                             # naturally extends from there.
.equ STACK_TOP,  0x90000     # protected-mode stack: between bootblock
                             # (0x7C00..) and kernel load area (0x100000).
.equ VBE_INFO,   0x0500      # scratch VbeInfoBlock (real-mode low RAM)
.equ MODE_INFO,  0x0700      # VBE ModeInfoBlock, read by the kernel

.code16
.global _start
_start:
	cli
	xor %ax, %ax
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %ss
	mov $0x7C00, %sp
	mov %dl, boot_drive        # BIOS left the boot drive in DL

	# --- VBE: query controller + a 1024x768 LFB mode (no mode switch yet) ---
	# Leaves the VBE ModeInfoBlock at MODE_INFO for the kernel to read.
	cld
	xor %ax, %ax
	mov %ax, %es
	mov $MODE_INFO, %di
	mov $256, %cx
	xor %al, %al
	rep stosb
	mov $VBE_INFO, %di
	movl $0x32454256, (%di)    # 'VBE2'
	mov $0x4F00, %ax
	int $0x10
	cmp $0x004F, %ax
	jne vbe_done
	xor %ax, %ax
	mov %ax, %es
	mov $MODE_INFO, %di
	mov $0x118, %cx
	mov $0x4F01, %ax
	int $0x10
	cmp $0x004F, %ax
	jne vbe_done
	mov $0x4118, %bx           # mode 0x118 | LFB
	mov $0x4F02, %ax
	int $0x10
vbe_done:

	# --- load the rest of the bootblock (sectors 1..BOOTBLOCK_SECTORS-1) ---
	# Single INT 13h read into 0x0000:LOAD_ADDR. 8 KiB never wraps the
	# 64 KiB real-mode segment boundary, so no chunked loop needed.
	movw $(BOOTBLOCK_SECTORS - 1), dap_count
	movw $LOAD_ADDR, dap_off
	movw $0x0000,    dap_seg
	movl $1,         dap_lba
	mov $dap, %si
	call read_dap

	# --- enable A20 (fast gate, port 0x92) ---
	in $0x92, %al
	or $0x02, %al
	and $0xFE, %al
	out %al, $0x92

	# --- enter protected mode ---
	cli
	lgdt gdt_descriptor
	mov %cr0, %eax
	or $0x1, %eax
	mov %eax, %cr0
	ljmp $0x08, $pm_entry

# Read the extended-read DAP at %si, retrying with a controller reset up to 3x.
read_dap:
	movb $3, retries
read_retry:
	mov boot_drive, %dl
	mov $0x42, %ah
	int $0x13
	jnc read_ok
	xor %ah, %ah               # reset disk system
	mov boot_drive, %dl
	int $0x13
	decb retries
	jnz read_retry
	jmp disk_error
read_ok:
	ret

disk_error:
	mov $'E', %al
	mov $0x0E, %ah
	int $0x10
hang16:
	hlt
	jmp hang16

# ---- data (sector 0) ----
boot_drive: .byte 0
retries:    .byte 0

.align 4
dap:
dap_size:   .byte 0x10
            .byte 0x00
dap_count:  .word 0
dap_off:    .word 0
dap_seg:    .word 0
dap_lba:    .long 0
dap_lba_hi: .long 0

# ---- GDT: flat 32-bit model ----
.align 8
gdt_start:
	.quad 0x0000000000000000   # null
gdt_code:                      # 0x08: base 0, limit 4 GiB, exec/read
	.word 0xFFFF
	.word 0x0000
	.byte 0x00
	.byte 0x9A
	.byte 0xCF
	.byte 0x00
gdt_data:                      # 0x10: base 0, limit 4 GiB, read/write
	.word 0xFFFF
	.word 0x0000
	.byte 0x00
	.byte 0x92
	.byte 0xCF
	.byte 0x00
gdt_end:

gdt_descriptor:
	.word gdt_end - gdt_start - 1
	.long gdt_start

	.fill 510-(.-_start), 1, 0
	.word 0xAA55

# ---- 32-bit protected-mode entry (loaded at LOAD_ADDR) ----
.code32
pm_entry:
	mov $0x10, %ax
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs
	mov %ax, %ss
	mov $STACK_TOP, %esp

	# zero the bootblock's .bss
	cld
	mov $__bss_start, %edi
	mov $__bss_end, %ecx
	sub %edi, %ecx
	xor %eax, %eax
	rep stosb

	call bootmain               # walks kernel ELF, copies PT_LOAD,
	                            # jumps to entry. Should not return.
hang32:
	cli
	hlt
	jmp hang32
