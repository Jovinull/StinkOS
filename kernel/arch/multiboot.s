# Multiboot-1 header so GRUB (or any other Multiboot-compliant loader)
# can boot kernel.elf without going through our own bootblock. Our
# native boot/bootmain.c ignores this section (it's just an unused
# PT_LOAD payload) -- the only reason it lives in kernel.elf is to
# open a future path to "install via GRUB on real hardware" as called
# out in TODO §0 (Real-PC boot).
#
# Spec: https://www.gnu.org/software/grub/manual/multiboot/multiboot.html
# Section 3.1 (header layout):
#   +0  magic    = 0x1BADB002
#   +4  flags    = ALIGN | MEMINFO (we don't need AOUT_KLUDGE; ELF works)
#   +8  checksum = -(magic + flags), so the sum of all three is zero
#
# The header must appear within the first 8192 bytes of the file and
# be aligned to a 4-byte boundary. boot/kernel.ld places .multiboot
# first in :text precisely so it satisfies both rules.

.section .multiboot, "a"
.align 4

.equ MB_MAGIC,    0x1BADB002
.equ MB_FLAGS,    0x00000003                   # ALIGN | MEMINFO
.equ MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

.long MB_MAGIC
.long MB_FLAGS
.long MB_CHECKSUM
