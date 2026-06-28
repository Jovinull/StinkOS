/* StinkOS physical/virtual memory map -- one place for every well-known
 * address the kernel and bootblock agree on. Mirror constants exist in
 * boot/boot.s and kernel/arch/kentry.s as raw `.equ` so the asm files
 * don't need a C preprocessor pass; if you change a value here, change
 * its asm twin too (each .equ has a comment pointing back here).
 *
 *   ┌──────────────────────────────────────────────────────────────┐
 *   │ 0x000000 .. 0x007BFF   BIOS data, IVT, real-mode scratch     │
 *   │ 0x007C00 .. 0x009FFF   Bootblock (boot.s + bootmain.o); dead │
 *   │                        once the kernel starts running.       │
 *   │ 0x00A000 .. 0x00FFFF   Free (small).                         │
 *   │ 0x010000 .. 0x01FFFF   Bootmain ELF-header scratch buffer.   │
 *   │ 0x020000 .. 0x08FFFF   Free (~448 KiB).                      │
 *   │ 0x080000 .. 0x090000   PM stack (grows down from STACK_TOP). │
 *   │ 0x090000 .. 0x0FFFFF   Free.                                 │
 *   │ 0x100000 .. ...        Kernel: .multiboot, .text, .rodata,   │
 *   │                        .data, .bss (linked by boot/kernel.ld)│
 *   │ 0x400000 .. 0x13FFFFF  User region (USER_PDES=4, 16 MiB)     │
 *   │ 0x10000000 ..          USER_FB_BASE (mapped on demand)       │
 *   │ 0x80000000 ..          KERNBASE: high-half direct map of     │
 *   │                        physical RAM [0, KERNEL_DIRECT_MAP).  │
 *   │                        Kernel dereferences phys frames via   │
 *   │                        P2V(phys) = phys + KERNBASE, which is │
 *   │                        ALWAYS kernel-only territory (user    │
 *   │                        page tables never reach above         │
 *   │                        USER_END = 0x1400000). xv6 pattern --  │
 *   │                        see osdev-refs/xv6-public/memlayout.h │
 *   │                        and vm.c for the canonical version.   │
 *   └──────────────────────────────────────────────────────────────┘
 */

#ifndef KERNEL_ARCH_MEMLAYOUT_H
#define KERNEL_ARCH_MEMLAYOUT_H

/* Bootblock + boot transition. */
#define BOOTBLOCK_BASE   0x00007C00u   /* BIOS-loaded sector 0 address */
#define BOOTMAIN_SCRATCH 0x00010000u   /* ELF header staging area */

/* Stack the kernel and bootmain both use after entering protected mode.
 * Grows downward; PM stack pages live in 0x80000..0x8FFFF. */
#define STACK_TOP        0x00090000u

/* Where the kernel ELF is linked (see boot/kernel.ld). */
#define KERNEL_LOAD_ADDR 0x00100000u

/* User address window. The kernel maps these on app exec and tears
 * them down on app exit. See kernel/sys/paging.h for the page-table
 * mechanics and kernel/sys/syscall.c for the user-pointer gate. */
#define USER_CODE_BASE   0x00400000u   /* user .text / .data / .bss */
#define USER_STACK_TOP   0x00540000u   /* user stack (grows down)   */
#define USER_HEAP_BASE   0x00540000u   /* sbrk/mmap start           */
#define USER_FB_BASE     0x10000000u   /* SYS_MAP_FB target         */

/* Higher-half kernel direct map (xv6-style).
 *
 * The kernel pgdir maps virt [KERNBASE, KERNBASE+KERNEL_DIRECT_MAP) ->
 * phys [0, KERNEL_DIRECT_MAP) via 4 MiB PSE PDEs. Every per-process
 * pgdir inherits these high-half PDEs, so the kernel can deref any
 * physical frame as `P2V(phys)` regardless of which proc is running
 * -- the high-half virt range sits ABOVE every per-proc user PT
 * window (USER_END = 0x1400000 << KERNBASE), so a user PT can never
 * shadow a kernel deref.
 *
 * KERNEL_DIRECT_MAP must cover all of pmm's physical range (today
 * PHYSTOP = 0x02000000 / 32 MiB; we reserve 256 MiB headroom).
 *
 * MMIO above DEVSPACE keeps the boot identity mapping (virt = phys)
 * for legacy drivers (fb LFB, e1000 BAR) -- xv6 does the same. */
#define KERNBASE            0x80000000u
#define KERNEL_DIRECT_MAP   0x10000000u   /* 256 MiB direct map      */
/* KERNEL_DEVSPACE must sit BELOW the lowest MMIO BAR we want to keep
 * identity-mapped. QEMU places the Bochs/Cirrus VBE LFB at 0xFD000000
 * and the e1000 BAR around 0xFEB80000; anchoring DEVSPACE at 0xFD000000
 * covers both with room to spare. xv6 picks 0xFE000000 for IOAPIC; we
 * pick lower because our VBE LFB is the first MMIO above the direct
 * map. */
#define KERNEL_DEVSPACE     0xFD000000u   /* identity-mapped MMIO    */

#ifndef __ASSEMBLER__
#define P2V(phys) ((unsigned int)(phys) + KERNBASE)
#define V2P(virt) ((unsigned int)(virt) - KERNBASE)
#endif

#endif
