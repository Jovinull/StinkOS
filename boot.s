# StinkOS bootloader
# Sector 1 (16-bit real mode): load kernel from disk via LBA, enable A20,
# load GDT, enter 32-bit protected mode, far-jump into the 32-bit entry.
# Sector 2+ holds the 32-bit entry stub and the kernel.

.equ KSECTORS, 40          # sectors to load from disk (kernel area, ~20 KiB)
.equ LOAD_ADDR, 0x7E00     # where the kernel area is loaded (right after boot)
.equ STACK_TOP, 0x90000    # protected-mode stack (below 640 KiB)
.equ VBE_INFO, 0x0500      # scratch VbeInfoBlock (real-mode, free low RAM)
.equ MODE_INFO, 0x0700     # VBE ModeInfoBlock, read by the kernel after PM

.code16
.global _start
_start:
	cli
	# flat real-mode segments, stack just under the boot sector
	xor %ax, %ax
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %ss
	mov $0x7C00, %sp
	mov %dl, boot_drive        # BIOS leaves the boot drive in DL

	# --- VBE: query controller + a 1024x768 LFB mode (no mode switch yet) ---
	# Leaves the VBE ModeInfoBlock at MODE_INFO for the kernel to read.
	cld
	xor %ax, %ax
	mov %ax, %es
	mov $MODE_INFO, %di         # zero the ModeInfoBlock first
	mov $256, %cx
	xor %al, %al
	rep stosb
	mov $VBE_INFO, %di          # request VBE 2.0 controller info
	movl $0x32454256, (%di)     # signature 'VBE2'
	mov $0x4F00, %ax
	int $0x10
	cmp $0x004F, %ax
	jne vbe_done
	xor %ax, %ax                # restore ES (BIOS may clobber it)
	mov %ax, %es
	mov $MODE_INFO, %di         # get info for mode 0x118 (1024x768, true colour)
	mov $0x118, %cx
	mov $0x4F01, %ax
	int $0x10
	cmp $0x004F, %ax
	jne vbe_done
	mov $0x4118, %bx            # set mode 0x118 | 0x4000 (linear framebuffer)
	mov $0x4F02, %ax
	int $0x10
vbe_done:
	# boot_drive was saved before VBE; the disk read reloads DL from it

	# --- load kernel sectors via INT 13h extended read (LBA), with retry ---
	movb $3, retries
load_disk:
	mov boot_drive, %dl
	mov $dap, %si
	mov $0x42, %ah
	int $0x13
	jnc load_ok
	xor %ah, %ah               # reset disk system
	mov boot_drive, %dl
	int $0x13
	decb retries
	jnz load_disk
	jmp disk_error
load_ok:

	# --- enable A20 (fast gate, port 0x92) ---
	in $0x92, %al
	or $0x02, %al
	and $0xFE, %al             # keep bit0 (fast reset) clear
	out %al, $0x92

	# --- enter protected mode ---
	cli
	lgdt gdt_descriptor
	mov %cr0, %eax
	or $0x1, %eax
	mov %eax, %cr0
	ljmp $0x08, $pm_entry      # load CS = code selector, flush, go 32-bit

disk_error:
	mov $'E', %al              # print 'E' then halt (real mode only)
	mov $0x0E, %ah
	int $0x10
hang16:
	hlt
	jmp hang16

# ---- data (sector 1) ----
boot_drive: .byte 0
retries:    .byte 0

.align 4
dap:                           # INT 13h Disk Address Packet
	.byte 0x10             # packet size
	.byte 0x00             # reserved
	.word KSECTORS         # sectors to read
	.word LOAD_ADDR        # dest offset
	.word 0x0000           # dest segment -> 0x0000:LOAD_ADDR
	.quad 1                # starting LBA (LBA 1 = 2nd sector)

# ---- GDT: flat 32-bit model ----
.align 8
gdt_start:
	.quad 0x0000000000000000   # null descriptor
gdt_code:                      # selector 0x08: base 0, limit 4 GiB, exec/read
	.word 0xFFFF
	.word 0x0000
	.byte 0x00
	.byte 0x9A
	.byte 0xCF
	.byte 0x00
gdt_data:                      # selector 0x10: base 0, limit 4 GiB, read/write
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
	mov $0x10, %ax             # data selector for all data segments
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs
	mov %ax, %ss
	mov $STACK_TOP, %esp

	# zero the .bss section (uninitialized statics) -- not stored in the image
	cld
	mov $__bss_start, %edi
	mov $__bss_end, %ecx
	sub %edi, %ecx
	xor %eax, %eax
	rep stosb

	call kernel_main
hang32:
	cli
	hlt
	jmp hang32
