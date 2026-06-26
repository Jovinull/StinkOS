# StinkOS bootloader
# Sector 1 (16-bit real mode): load kernel from disk via LBA, enable A20,
# load GDT, enter 32-bit protected mode, far-jump into the 32-bit entry.
# Sector 2+ holds the 32-bit entry stub and the kernel.

.equ KSECTORS, 127         # sectors to load from disk (kernel area, ~64 KiB);
                           # must exceed the linked kernel image (boot+code+data,
                           # up to __bss_start) and stay below APP1_LBA (128).
.equ LOAD_ADDR, 0x7E00     # where the kernel area is loaded (right after boot)
# A single INT 13h read to 0x0000:LOAD_ADDR would wrap at the 64 KiB segment
# boundary (0xFFFF) once the kernel exceeds ~33 KiB, scribbling over low RAM.
# So load in chunks that never cross a segment: the first fills LOAD_ADDR..0x10000
# in segment 0, then each following chunk loads 64 KiB into the next segment
# (0x1000, 0x2000, ...). The image stays contiguous in linear memory, so the
# kernel's link address is unchanged and the kernel may grow past 64 KiB, up to
# the low-memory stack. Loop state lives in memory because INT 13h is not
# guaranteed to preserve registers across the call.
# NB: '/' is a line-comment char in this assembler, so divide by 512 with '>>9'.
.equ STAGE1, (0x10000 - LOAD_ADDR) >> 9    # 65 sectors: exactly fills up to 0x10000
.equ CHUNK,  128                           # 64 KiB chunks after the first (seg += 0x1000)
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

	# --- load the kernel: first chunk to LOAD_ADDR, then 64 KiB chunks ---
	movw $STAGE1, dap_count
	movw $LOAD_ADDR, dap_off
	movw $0x0000, dap_seg
	movl $1, dap_lba
	mov $dap, %si
	call read_dap
	movw $(KSECTORS - STAGE1), remaining
	movw $0x1000, cur_seg
	movl $(1 + STAGE1), cur_lba
load_loop:
	movw remaining, %ax
	test %ax, %ax
	jz load_ok
	cmp $CHUNK, %ax
	jbe 1f
	mov $CHUNK, %ax
1:
	movw %ax, chunk_n
	movw %ax, dap_count
	movw $0x0000, dap_off
	movw cur_seg, %bx
	movw %bx, dap_seg
	movl cur_lba, %ebx
	movl %ebx, dap_lba
	mov $dap, %si
	call read_dap
	movw chunk_n, %ax
	movw remaining, %bx
	sub %ax, %bx
	movw %bx, remaining
	movzwl %ax, %eax
	addl %eax, cur_lba
	addw $0x1000, cur_seg
	jmp load_loop

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
# Disk Address Packet, rewritten in place for each chunk by the load loop.
dap:
dap_size:   .byte 0x10         # packet size
            .byte 0x00         # reserved
dap_count:  .word 0            # sectors to read this chunk
dap_off:    .word 0            # destination offset
dap_seg:    .word 0            # destination segment
dap_lba:    .long 0            # starting LBA, low 32 bits
dap_lba_hi: .long 0            # starting LBA, high 32 bits (kernel is low, =0)

# Load-loop state (kept in memory; INT 13h may clobber registers).
remaining:  .word 0            # sectors still to load after the first chunk
cur_seg:    .word 0            # destination segment for the next chunk
cur_lba:    .long 0            # starting LBA for the next chunk
chunk_n:    .word 0            # sectors in the current chunk

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

	call kmain
hang32:
	cli
	hlt
	jmp hang32
