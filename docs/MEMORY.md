# Memory Accounting and Leak Sweeps

StinkOS has two layers of memory bookkeeping the kernel exposes to userland
or to a test harness. Use them together when chasing "the kernel slowly
drips frames after every app exit" regressions.

## Layer 1: PMM frame counts

The physical memory manager (`kernel/arch/pmm.c`) tracks total and free
frames over the managed range: from 1 MiB up to `PMM_PHYS_LIMIT` (64 MiB),
or up to the amount of RAM the firmware reports in CMOS when the machine
has less. Userland reads the numbers via `SYS_MEMINFO` (`lib/libstink.h`
-> `sys_meminfo`); the shell's `mem` command and the System Info window
print them.

```
> mem
phys: <used>/<total> pages (<used> KiB used / <total> KiB total)
user brk: 0x540000
```

The kernel does not read a firmware memory map: it assumes everything
between 1 MiB and the top it computed is usable RAM, which holds on QEMU
and on ordinary PCs with at least 64 MiB.

Out-of-memory is loud: the first allocation past the watermark emits a
one-shot `pmm: OUT OF MEMORY (all N pages handed out)` line to the serial
log and `pmm_alloc` returns `0`. Subsequent failures are silent so a
runaway loop doesn't drown the log. A process that hits it gets a failure
from `sbrk` / `mmap` and keeps everything it had (`ktest sbrk-oom`, `oom`).

## Layer 2: per-app reset audit

On every app exit, `paging_reset_user_heap()` walks the user heap region,
unmaps each present page, and emits:

```
paging: reclaimed 17 user heap frames
```

The number is whatever the previous app accumulated through `paging_user_alloc`
(triggered by `sys_alloc` / `sys_sbrk` / `sys_mmap`). If that number does
not match the app's own malloc/sbrk accounting on the way down, something
in the kernel reset path leaked.

`paging_user_mapped_pages()` (in `paging.h`) returns the count of present
PTEs in the user region right now. Call it from a custom test harness to
assert "after exit, zero user pages remain mapped".

## Running a sweep

The automated sweep is part of `make ktest`. Each of these cases reads
the free-frame count, does something many times, and fails if the count
did not come back:

| Case | Cycle |
|---|---|
| `leak-fork` | fork + exit + wait |
| `leak-exec` | fork + exec a second image + wait |
| `kill-reap` | fork, kill the child, wait |
| `orphan` | a child whose parent exits first |
| `pmm-churn` | allocate and free more frames than exist, repeatedly |
| `win-leak` | create, resize and destroy windows; exit and exec with a window open |
| `audio-unmap` | play sounds and let them end, or exit while they play |

The manual version, for chasing a new one:

1. Boot StinkOS with serial logging on:
   `qemu-system-i386 -snapshot -drive format=raw,file=os.bin -serial stdio`
2. Note the `pmm:` and `paging: reclaimed` lines emitted at idle.
3. Launch an app, exercise it, exit, repeat.
4. The free-page total visible via `mem` should plateau, not creep
   downward.

If the total trends downward across cycles, look for a kernel-side
owner that outlives the process: every such owner has a release hook
called from process exit (`win_proc_exit`, `audio_reclaim`,
`pipe_release_all`, `tcp_close_pid`), and a new kind of
kernel-owned memory needs one too.

## Higher-half kernel: why every phys deref goes through P2V

Pre-v0.4 the kernel identity-mapped 4 GiB of physical RAM and each
process pgdir replaced PDEs `[1..4]` (virt `0x400000..0x1400000`) with
4 KiB user page tables. The kernel routinely dereferenced raw physical
frames as `(T *)phys` -- safe under the assumption "identity-map means
virt == phys". The assumption was wrong: when the active CR3 belongs to
a user proc, any kernel write to a phys address that happens to fall in
`[USER_BASE, USER_END)` walks through that proc's user PT instead of
the 4 MiB PSE identity. If `pmm_alloc` returned (say) phys `0x402000`
to `copy_user_pgdir` while the parent's pgdir was active, the kernel
zeroed parent's user `.rodata` and the next user-mode `strcmp` faulted
at a null-deref deep inside the shell.

The fix (commit `a506709`, branch `feat/higher-half-kernel`) follows
the xv6 pattern:

- Kernel image links at virt `0x80100000` (LMA `0x100000`).
- Each pgdir maps `[KERNBASE, KERNBASE + 256 MiB)` as a high-half
  direct map of phys `[0, 256 MiB)` via 4 MiB PSE PDEs.
- The runtime kernel pgdir has **no** entries below `KERNBASE` except
  the per-process user PTs at PDE`[1..4]`. There is no identity-low
  to alias.
- Every kernel deref of a physical frame goes through `KVA(p) =
  (T *)(p + KERNBASE)`, landing in the high-half mirror. Above
  `KERNBASE` the kernel pgdir is exclusive territory -- user PTs
  cannot reach there because `USER_END = 0x1400000` < `KERNBASE`.
- Drivers that hand physical addresses to hardware (audio DMA, e1000
  RX/TX rings + descriptor base registers, ata Bus Master PRDT)
  apply `V2P()` to convert their BSS-resident virt buffers back to
  phys. Device memory (the linear framebuffer, the e1000 registers) is
  mapped explicitly with `paging_map_mmio`, wherever the firmware put
  it; the kernel never uses a device's physical address as a pointer.

The bug class is now impossible by construction: no kernel deref ever
touches the `[0, KERNBASE)` virt range, and no user PT ever reaches
above `KERNBASE`. Refs: `osdev-refs/xv6-public/memlayout.h` (P2V/V2P
macros), `vm.c:316` (copyuvm using P2V).
