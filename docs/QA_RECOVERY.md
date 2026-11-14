# StinkOS QA recovery log

A record of the stabilisation pass that started on 2026-11-14: what was
audited, what was found, what was fixed and how each claim was checked.
It is a working engineering log, not release notes. Every entry carries
one of these labels:

| Label | Meaning |
|-------|---------|
| **Fixed + retested** | Reproduced by a test, fixed, the test now passes. |
| **Verified** | Checked by execution and found correct. |
| **Not reproduced** | Suspected from reading the code; no failing run. |
| **Not tested** | Not exercised by anything in this pass. |
| **By design** | Known limitation the project accepts. |
| **Future** | Real problem, deliberately not addressed here. |

## Environment

| | |
|---|---|
| Host | Fedora 45, x86_64, gcc 16.2.1 |
| Cross toolchain | `i386-elf` binutils 2.43 + gcc 14.2.0 (same versions as CI) |
| Rust | nightly + `rust-src` |
| Emulator | `qemu-system-i386` 11.1, `-cpu Penryn` (NX), default 128 MiB |
| Baseline `master` | `a7294f8` |
| Baseline PR #16 (`feat/userland-ui`) | `919878a` |

## How to reproduce

```sh
make all                      # build os.bin
make unittest                 # host-side unit tests
make test-headless            # boot + menu + legacy apps
python3 tools/qa/ktest.py     # kernel regression cases, one VM per case
```

`tools/qa/ktest.py` boots a private copy of `os.bin` per case, with an
`AUTORUN` file that makes the kernel menu start `KTEST.ELF` at boot. The
app logs a verdict on serial and powers the VM off, so the runner waits on
QEMU exiting rather than on a sleep. `os.bin` itself is never written.

## 1. Kernel core (affects `master`)

### 1.1 Baseline

`apps/ktest.c` was written first and run against the unmodified kernel
code of `master`. Result: **1 of 14 cases passed.**

| Case | Result on `master` |
|------|--------------------|
| `leak-fork` | 301 frames lost over 30 fork+exit cycles |
| `leak-exec` | 6680 frames lost over 20 fork+exec cycles (of 8192 total) |
| `pmm-churn` | 1024 frames lost after one `munmap` of 2048 pages |
| `heap-switch` | parent's program break moved after its child exec'd |
| `cow-kwrite` | child's `fread` landed in the parent's page |
| `text-kwrite` | kernel overwrote read-only `.text` shared with the parent |
| `munmap-hole` | kernel page fault, machine halted |
| `kill-reap` | 55 frames lost over 5 kill+reap cycles |
| `orphan` | 85 frames lost, zombies never reaped |
| `sleep-fair` | 4 sleeping processes cut a busy one to 4 % of its CPU |
| `preempt` | pass |
| `concurrent` | file contents corrupted with two processes writing |
| `sbrk-oom` | break moved after a failed `sbrk` |
| `badptr` | kernel page fault, machine halted |

### 1.2 Findings and fixes

All **Fixed + retested** (`ktest` 15/15 after the fixes; the 15th case,
`resched`, was added while fixing).

**K1 (P0) — the kernel preempted itself with no locking.**
`syscall_dispatch` re-enabled interrupts on entry and the PIT handler
called `proc_yield()` unconditionally, so a process could be switched out
in the middle of a filesystem write, a frame allocation or a page-table
update while another process entered the same code. `concurrent` shows the
result as corrupted file contents.
Fix: one scheduling rule. A switch happens when a tick interrupts ring 3,
on the way back to ring 3 after a tick that landed in kernel mode
(`proc_resched_point`), or where kernel code explicitly yields or blocks.
Interrupts stay enabled in syscalls; interrupt handlers are the only
concurrent writers left.

**K2 (P0) — one program break for every process.**
`user_heap_next` and `fb_pde_mapped` were globals in `paging.c`, reset by
whichever process exec'd last. After a child exec'd, the parent's `sbrk`
returned addresses inside its own live heap and the kernel rejected
buffers in the parent's heap. Fix: both are saved into the PCB and
restored around every switch, and carried across fork and a failed exec.

**K3 (P0) — the kernel wrote through COW and read-only pages.**
`CR0.WP` was never set, so a ring-0 write ignored the R/W bit. A child
passing a still-shared buffer to `fread` had the data land in the frame it
shared with its parent; passing a `.text` address overwrote code shared by
every fork of the image. Fix: `CR0.WP` is set at `paging_init`; syscalls
that write to user memory use `paging_user_range_writable`, which
privatizes COW pages first and refuses pages that are genuinely
read-only. A ring-0 write fault on a COW user page is still resolved in
the fault handler (and logged) in case a call site was missed.

**K4 (P0) — pointer validation by address range.**
`paging_user_range_ok` accepted anything between the heap base and the
break, including holes left by `munmap`; the kernel then dereferenced an
unmapped page and halted. Fix: validation walks the live page tables
(present + user on every page).

**K5 (P1) — forked processes were never freed.**
`proc_reap` released the kernel stack only. The page directory, page
tables and every private frame of each fork stayed allocated: ~334 frames
per fork+exec, so roughly 24 launches exhausted a 32 MiB machine.
Fix: reaping destroys the address space. Zombies with no parent are
reaped by the kernel (`proc_reap_orphans`); a dying process orphans its
living children and reaps its zombie ones.

**K6 (P1) — the frame free list dropped frames.**
`pmm_free` pushed onto a 1024-entry array and discarded the rest. Any
release above 4 MiB lost frames until reboot. Fix: the free list is a
stack threaded through a per-frame index table. `tests/test_pmm.c` now
links the real `kernel/arch/pmm.c`.

**K7 (P1) — sleeping processes consumed CPU.**
`sleep`, `wait` and pipe I/O looped on `sti; hlt; cli` as READY
processes, each taking a full time slice per tick. Fix: real blocking
(`proc_sleep_ticks`, `proc_block`, `proc_wake`); the PIT wakes timed
sleepers, a child's exit wakes a waiting parent.

**K8 (P1) — `SYS_KILL` tore the target down from the killer's context.**
The target was marked ZOMBIE on the spot, possibly in the middle of a
syscall on its own kernel stack, and its resources were released with the
killer's address space loaded. Fix: kill sets a flag and wakes the
target, which exits itself at its next return to ring 3.

**K9 (P1) — 4 KiB kernel stacks.** Every forked process got a single PMM
frame as its kernel stack, with syscall handlers placing 256–512 byte
buffers on it and interrupts nesting on top. Fix: 8 KiB stacks from a
static per-slot pool, a canary checked on every switch and at reap, and a
log line when a process used more than 75 % of its stack.

**K10 (P2) — `sbrk` kept a partial mapping on failure**, and an oversized
request silently grew the heap to its limit before reporting -1. Now
all-or-nothing.

**K11 (P2) — user memory was not zeroed.** Fresh code, stack and heap
frames carried whatever the previous owner left. Now zeroed at mapping.

**K12 (P2) — a failed `exec` jumped to the menu.** `exec_run_by_elf`
stages the new image in a separate address space and rolls back, but the
syscall then called `menu_exit()` anyway — from a child's kernel stack
when the caller was a forked process. It now returns -1.

**K13 (P2) — `exec_active` was set by any process.** The "go back to the
shell when this app ends" flag is meant for the foreground app in PID 1;
any forked child that exec'd flipped it. Now only PID 1 does.

**K14 (P2) — a faulting child reported exit code 0** to its parent's
`wait`. Now 139.

### 1.3 Regression status after the kernel fixes

| Suite | Result |
|-------|--------|
| `tools/qa/ktest.py` | 15/15 |
| `make unittest` | 64 binaries, 0 failures |
| `make test-headless` | pass |
| `smoke-multiproc`, `-acpi`, `-cow`, `-vfs-mounts`, `-wxattack`, `-exitcode` | pass |
| `smoke-rs-hello`, `-alloc`, `-stdio`, `-json`, `-life` | pass |

### 1.4 Known limits

- **By design:** the PMM manages a fixed range (`PMM_PHYS_LIMIT`); the
  kernel does not read a firmware memory map. The machine needs at least
  that much RAM.
- **By design:** the kernel is non-preemptive, so a long syscall (reading
  a 28 MiB WAD through PIO) holds every other process for its duration.
- **Future:** several `tests/test_*.c` still test a copy of kernel logic
  rather than the kernel's own source file.

## 2. Audit backlog (working list)

Findings from the read-through of audio, storage, boot and drivers. Each
is reproduced by a test before it is fixed; this list shrinks as entries
move into the sections above.

| Id | Sev | Area | Finding | Status |
|----|-----|------|---------|--------|
| A1 | P0 | audio | IRQ mixer dereferences user sample pointers under whatever address space is loaded; unmap/exec/another process running halts the kernel | open |
| A2 | P0 | fs | `SYS_MOUNT` accepts any LBA range: a mount over LBA 0 lets an app overwrite the boot sector and kernel | open |
| A3 | P1 | pipes | no owner, not closed on exit, not counted across fork | open |
| A4 | P0 | traps | no `cld` on trap entry (user DF=1 reverses kernel `rep movs`); fork seeds a ring-0 `popf` from user EFLAGS | **fixed + retested** (`ktest eflags`) |
| A5 | P1 | input | key queue never flushed: keys typed in an app replay into the menu (can launch apps, delete files) | open |
| A6 | P1 | video | `SYS_MAP_FB` maps 2 MiB of a 2.25-3 MiB framebuffer; userland assumes 32 bpp, QEMU's mode 0x118 is 24 bpp | open |
| A7 | P2 | input | mouse accumulators read-and-cleared with IRQs on; cursor save/draw not atomic | open |
| A8 | P2 | rtc | unbounded update-in-progress loop; CMOS index raced by IRQ8 | open |
| A9 | P0 | acpi | firmware table pointers above the 256 MiB direct map fault before the IDT exists (`-m 512`) | open |
| A10 | P1 | video | kernel uses the LFB physical address as a virtual one; only valid at or above 0xFD000000 | open |
| A11 | P1 | boot | stage 2 reads the kernel from the primary ATA master whatever the BIOS boot drive was | open |
| A12 | P1 | pmm | no RAM detection: with less RAM than `PMM_PHYS_LIMIT` the PMM hands out frames that do not exist | open |
| A13 | P1 | boot | one hard-coded VBE mode, set-mode result ignored, no fallback | open |
| A14 | P2 | menu | `klongjmp` resumes with ESP 4 bytes low (masked by frame pointers) | fixed (latent, no failing run) |
| A15 | P2 | input | CapsLock toggles on typematic repeat; one flag for both Shifts / both Ctrls; 0xE1 not handled | open |
| A16 | P2 | signals | PID 1 keeps pending signals and handlers across apps; signal 0 is accepted but unreportable | open |
| A17 | P2 | paging | W^X only covers the first 2 MiB of the kernel image; nothing asserts `.text`/`.rodata` stay inside | open |
| A18 | P1 | audio | Q16.16 cursor wraps at 65536 samples: longer sounds loop forever and never free the channel | open |
| A19 | P1 | audio | mixer handles have no owner: any process can stop or mute another's channels | open |
| A20 | P1 | audio | stereo mode programs half the block size (per QEMU's sb16 model) | open |
| A21 | P2 | audio | `SYS_AUDIO_MODE` stops output before validating the mode; DMA ring not cleared on format change | open |
| A22 | P2 | speaker | tone outlives the process that started it | open |
| A23 | P1 | fs | new-file path advances `next_free` before the directory-full check | open |
| A24 | P2 | fs | case-sensitive lookups everywhere except the loader | open |
| A25 | P2 | vfs | `vfs_seek` adds offsets as signed int with no overflow guard | open |
| A26 | P2 | misc | unbounded 8042 wait in `SYS_REBOOT` | open |
| N1 | P0 | net | `tcp_emit_rst` keeps an 11 KiB `struct tcb` on an 8 KiB kernel stack; the TX call chain alone nests four 1.5 KiB buffers | open |
| N2 | P0 | net | the whole RX path (ARP/IP/TCP, replies included) runs inside the IRQ handler, on top of any syscall using the same structures and static buffers | open |
| N3 | P0 | net | socket handles have no owner: any process can read, write or close another's connection | open |
| N4 | P1 | net | e1000 TX slot reused by an IRQ-context send; RX ring cursor raced between poll and IRQ drain | open |
| N5 | P1 | tcp | `tcp_drain_tx` re-sends unacknowledged bytes under new sequence numbers | open |
| N6 | P1 | tcp | a pure ACK carrying options has its option bytes delivered as stream data | open |
| N7 | P1 | tcp | one RST or half-open SYN frees a listening socket | open |
| N8 | P1 | tcp | window updates ignored unless they also acknowledge data (zero-window stall) | open |
| N9 | P1 | tcp | FIN handling: jumps holes, close drops unsent data, FIN retransmitted at the wrong sequence | open |
| N10 | P1 | dhcp | options applied in any state, constant xid, no chaddr check | open |
| N11 | P2 | net | ARP entry not refreshed by use; DNS/ping state global; UDP/TCP checksum pseudo-header uses the local IP instead of the packet's destination; timers only run when userland polls | open |
