# Memory Accounting and Leak Sweeps

StinkOS has two layers of memory bookkeeping the kernel exposes to userland
or to a test harness. Use them together when chasing "the kernel slowly
drips frames after every app exit" regressions.

## Layer 1: PMM frame counts

The physical memory manager (`kernel/arch/pmm.c`) tracks total and free
frames over the managed range (1 MiB .. 32 MiB today). Userland reads the
numbers via `SYS_MEMINFO` (`lib/libstink.h` -> `sys_meminfo`); the shell's
`mem` command prints them.

```
> mem
phys: 384/8192 pages (1536 KiB used / 32768 KiB total)
user brk: 0x540000
```

Out-of-memory is loud: the first allocation past the watermark emits a
one-shot `pmm: OUT OF MEMORY (all N pages handed out)` line to the serial
log and `pmm_alloc` returns `0`. Subsequent failures are silent so a
runaway loop doesn't drown the log.

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

The minimum-effort manual sweep:

1. Boot StinkOS with serial logging on:
   `qemu-system-i386 -drive format=raw,file=os.bin -serial stdio`
2. Note the `pmm:` and `paging: reclaimed` lines emitted at idle.
3. Launch an app, exercise it, exit, repeat.
4. Each cycle should print one matching `reclaimed N` line; the running
   free-page total visible via `mem` should plateau, not creep downward.

If you see the total trend downward across cycles, the suspect order is:

1. `paging_user_mmap` not paired with `paging_user_munmap` (deliberate --
   munmap leaves the bump pointer in place; the reset zeros it).
2. A kernel-side cache that holds a `pmm_alloc`'d frame without freeing
   on app teardown (e.g. a stale DMA buffer, an open VFS descriptor
   pinning a sector, an unwound `sys_mmap` mid-allocation).
3. A page-table allocation in `paging_init_user` that never gets revisited
   (one-time on boot; shows up as a constant overhead, not a per-cycle
   drip).

The sweep doesn't ship as a Make target yet because it needs interactive
input. Adding one is straightforward: spawn / exit an app N times via the
test-headless harness, parse the serial log for the reclaimed counts, and
fail if the post-cycle free total dips below the pre-cycle baseline by
more than tolerance (a small constant drift is expected because the heap
high-water mark inside an app may not match across runs).
