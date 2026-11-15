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
| Emulator | `qemu-system-i386` 11.1, `-cpu Penryn` (NX), default 128 MiB, TCG. Every suite built on `tools/qa/stinkqa.py` was also run with `STINKQA_KVM=1` (`-enable-kvm -cpu host`) |
| Baseline `master` | `a7294f8` |
| Baseline PR #16 (`feat/userland-ui`) | `919878a` |

## How to reproduce

```sh
make all            # build os.bin (tools/fetch-wads.sh first, for Doom)
make unittest       # host-side tests
make qa             # everything that boots: ktest, ktest-lowmem, qa-net,
                    # qa-input, qa-kbd, qa-wm, qa-apps, qa-doom, qa-install
make test-headless  # the text menu and the console programs
STINKQA_KVM=1 make qa   # the same on the host CPU
```

Every script boots a private copy of the image or uses `-snapshot`;
`os.bin` is never written. `docs/TESTING.md` describes each suite.

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

Section 6 has the results for the whole tree. On the kernel-only branch
(`fix/kernel-recovery`, PR #17): `ktest` 25/25, `ktest --mem 24` on the
memory cases, `qa-net`, `qa-input`, `qa-kbd`, `make unittest`,
`test-headless` and the eleven `smoke-*` scripts pass, locally and in CI.

### 1.4 Known limits

- **By design:** the PMM manages one contiguous range, from 1 MiB to the
  smaller of `PMM_PHYS_LIMIT` and the RAM size in CMOS. The kernel does
  not read a firmware memory map, so a reserved hole inside that range
  would be handed out as RAM. QEMU and ordinary PCs have none there.
- **By design:** the kernel is non-preemptive, so a long syscall holds
  every other process for its duration. The longest ones are disk-bound:
  reading a large file in one call, and deleting a file that has a lot
  of data stored after it (the delete compacts the disk).
- **Future:** several `tests/test_*.c` still test a copy of kernel logic
  rather than the kernel's own source file.

## 2. Audit backlog (working list)

Findings from the read-through of audio, storage, boot and drivers. Each
is reproduced by a test before it is fixed; this list shrinks as entries
move into the sections above.

| Id | Sev | Area | Finding | Status |
|----|-----|------|---------|--------|
| A1 | P0 | audio | IRQ mixer dereferences user sample pointers under whatever address space is loaded; unmap/exec/another process running halts the kernel | **fixed + retested** (`ktest audio-unmap`: kernel halt before): the mixer owns a copy of every sound |
| A2 | P0 | fs | `SYS_MOUNT` accepts any LBA range: a mount over LBA 0 lets an app overwrite the boot sector and kernel | **fixed + retested** (`ktest mount-guard`); on-disk directories are also validated at mount |
| A3 | P1 | pipes | no owner, not closed on exit, not counted across fork | **fixed + retested** (`ktest pipes`): per-process ownership bits, references across fork, release at exit |
| A4 | P0 | traps | no `cld` on trap entry (user DF=1 reverses kernel `rep movs`); fork seeds a ring-0 `popf` from user EFLAGS | **fixed + retested** (`ktest eflags`) |
| A5 | P1 | input | key queue never flushed: keys typed in an app replay into the menu (can launch apps, delete files) | **fixed + retested** (`tools/qa/input_flush.py`) |
| A6 | P1 | video | `SYS_MAP_FB` maps 2 MiB of a 2.25-3 MiB framebuffer; userland assumes 32 bpp, QEMU's mode 0x118 is 24 bpp | **fixed + retested**: the whole LFB is mapped, `SYS_FB_INFO` reports pitch and depth, `fbdemo` verified at 24 bpp (cirrus) and 32 bpp |
| A7 | P2 | input | mouse accumulators read-and-cleared with IRQs on; cursor save/draw not atomic | fixed (read-and-clear and cursor snapshot with IRQs off); no failing run |
| A8 | P2 | rtc | unbounded update-in-progress loop; CMOS index raced by IRQ8 | fixed (bounded wait, CMOS access with IRQs off); no failing run |
| A9 | P0 | acpi | firmware table pointers above the 256 MiB direct map fault before the IDT exists (`-m 512`) | **fixed + retested** (`ktest --mem 512`: boot hung at the RSDP line before) |
| A10 | P1 | video | kernel uses the LFB physical address as a virtual one; only valid at or above 0xFD000000 | **fixed + retested**: LFB and NIC registers mapped through `paging_map_mmio` (`-vga cirrus` halted at the first pixel before) |
| A11 | P1 | boot | stage 2 reads the kernel from the primary ATA master whatever the BIOS boot drive was | open |
| A12 | P1 | pmm | no RAM detection: with less RAM than `PMM_PHYS_LIMIT` the PMM hands out frames that do not exist | **fixed + retested** (`ktest --mem 24 oom`: a second process was given memory that did not hold data): the PMM range is clamped to the RAM size in CMOS. Not a memory map: reserved holes below the top are still assumed absent |
| A13 | P1 | boot | one hard-coded VBE mode, set-mode result ignored, no fallback | **fixed + retested**: the bootloader scans the VBE mode list for 1024x768 (32 bpp, then 24), checks the set-mode result and falls back; verified on QEMU std, cirrus and vmware |
| A14 | P2 | menu | `klongjmp` resumes with ESP 4 bytes low (masked by frame pointers) | fixed (latent, no failing run) |
| A15 | P2 | input | CapsLock toggles on typematic repeat; one flag for both Shifts / both Ctrls; 0xE1 not handled | **fixed + retested** for the two-Shift case (`tools/qa/kbd_test.py`: typed `Xxx`, want `XXx`); Caps Lock repeat and 0xE1 are fixed but QEMU key injection does not reproduce them |
| A16 | P2 | signals | PID 1 keeps pending signals and handlers across apps; signal 0 is accepted but unreportable | fixed (handlers and pending signals cleared on exec / app launch, signal 0 refused); not covered by a test |
| A17 | P2 | paging | W^X only covers the first 2 MiB of the kernel image; nothing asserts `.text`/`.rodata` stay inside | fixed (linker ASSERT) |
| A18 | P1 | audio | Q16.16 cursor wraps at 65536 samples: longer sounds loop forever and never free the channel | **fixed + retested** (`ktest audio-modes`, 70000-sample sound) |
| A19 | P1 | audio | mixer handles have no owner: any process can stop or mute another's channels | **fixed + retested** (`ktest audio-modes`): handles carry owner + generation |
| A20 | P1 | audio | stereo mode programs half the block size (per QEMU's sb16 model) | **fixed + retested** (`ktest audio-modes`: 1.00 s of audio took 480 ms) |
| A21 | P2 | audio | `SYS_AUDIO_MODE` stops output before validating the mode; DMA ring not cleared on format change | **fixed + retested** (`ktest audio-modes`) |
| A22 | P2 | speaker | tone outlives the process that started it | fixed (speaker released when its owner exits; divisor clamped); not covered by a test |
| A23 | P1 | fs | new-file path advances `next_free` before the directory-full check | fixed; not covered by a test |
| A24 | P2 | fs | case-sensitive lookups everywhere except the loader | open |
| A25 | P2 | vfs | `vfs_seek` adds offsets as signed int with no overflow guard | fixed; not covered by a test |
| A26 | P2 | misc | unbounded 8042 wait in `SYS_REBOOT` | fixed |
| N1 | P0 | net | `tcp_emit_rst` keeps an 11 KiB `struct tcb` on an 8 KiB kernel stack; the TX call chain alone nests four 1.5 KiB buffers | **fixed + retested** (`tools/qa/net_rst.py`: 8048 of 8192 stack bytes used before, audit quiet after) |
| N2 | P0 | net | the whole RX path (ARP/IP/TCP, replies included) runs inside the IRQ handler, on top of any syscall using the same structures and static buffers | **fixed + retested**: RX and timers moved to the `knet` kernel thread (`ktest net-ping`) |
| N3 | P0 | net | socket handles have no owner: any process can read, write or close another's connection | fixed (syscalls gate on `tcp_owned_by`); not yet covered by a test |
| N4 | P1 | net | e1000 TX slot reused by an IRQ-context send; RX ring cursor raced between poll and IRQ drain | fixed by N2 (no ring access from IRQ context); TX wait is bounded |
| N5 | P1 | tcp | `tcp_drain_tx` re-sends unacknowledged bytes under new sequence numbers | open |
| N6 | P1 | tcp | a pure ACK carrying options has its option bytes delivered as stream data | open |
| N7 | P1 | tcp | one RST or half-open SYN frees a listening socket | open |
| N8 | P1 | tcp | window updates ignored unless they also acknowledge data (zero-window stall) | open |
| N9 | P1 | tcp | FIN handling: jumps holes, close drops unsent data, FIN retransmitted at the wrong sequence | open |
| N10 | P1 | dhcp | options applied in any state, constant xid, no chaddr check | open |
| N11 | P2 | net | ARP entry not refreshed by use; DNS/ping state global; UDP/TCP checksum pseudo-header uses the local IP instead of the packet's destination; timers only run when userland polls | partly fixed: the timers run in `knet` whatever userland does (N2). The rest is open |
| K15 | P1 | sched | x87 registers were not saved across context switches: two processes using floating point corrupted each other's values; the FPU was never initialised | **fixed + retested** (`ktest fpu`: "parent's x87 registers changed under it") |
| K16 | P1 | proc | `SYS_PROC_INFO` returned only its header line (padding loop never advanced): `ps` showed no processes | **fixed + retested** (`ktest procinfo`) |
| K17 | P1 | sched | round-robin restarted from the slot of whichever high-priority thread ran last, starving all but one lower-priority process | **fixed + retested** (`ktest fair3`, `win-exec-kill` on the userland branch hung) |
| A27 | P2 | fs | one ATA command (and, for writes, one cache flush) per sector in every file operation | **fixed + retested** (section 5): runs of up to 128 sectors; `ktest fs-runs` |
| A28 | P2 | fs | a delete moves every sector stored after the file, with nothing to recover from a failure half way | open (documented in `docs/STINKFS.md`); the move itself is now 128 sectors per command |
| A29 | P2 | ata | completion is detected by polling the status port with the CPU held | open: correct, but slow where a port read is expensive (measured under KVM, section 5) |
| K18 | P0 | proc | kernel threads could be suspended, killed, signalled or demoted by any process | **fixed + retested** (`ktest kthread-guard`; section 3.6 R2) |
| B1 | P1 | build | no header dependencies for kernel objects | fixed (`-MMD -MP`) |

## 3. Graphical userland (PR #16)

### 3.1 The PR as submitted (`919878a`)

| Observation | Evidence |
|---|---|
| CI red since the commit that made the desktop start at boot | GitHub Actions runs of 2026-07-01 on `feat/userland-ui`: the text-menu scripts found a desktop instead of the menu |
| The drawing primitives crash on hostile coordinates | a host harness with 4096-word guard bands around the buffer: `win_fillrect_buf(x = -1)` dies with SIGSEGV on the first call |
| Stale pixels, a second pointer, empty title bar, overlapping captions | `images/qa/pr16_before_calc.png`: the calculator from the original tree, one screenshot |

<p align="center">
  <img src="../images/qa/pr16_before_calc.png" width="48%" alt="The original PR: calculator window with an empty title bar, two pointers and overlapping desktop captions">
  <img src="../images/desktop_windows.png" width="48%" alt="The same system after the pass: four overlapping windows with kernel-drawn chrome and a taskbar">
  <br><sub>Before (as submitted) and after.</sub>
</p>

The design had the compositor inside the timer interrupt, drawing every
window straight onto the visible framebuffer; windows painted their own
frames; all programs read one global key queue and rebuilt a pointer
position from raw deltas; the launcher waited for each program to end.
Patching that in place would have left the causes. The window system
was rebuilt (`188abae`) and the fourteen programs ported to it; what
follows answers each of the investigations the pass was asked to make.

### 3.2 The seven investigations

**G1 — writes outside graphics buffers (`libstink_winbuf`). Fixed + retested.**
The primitives took a pointer and a width, no height, and clipped
nothing: a negative x, a rectangle past the bottom or a string near the
edge wrote outside the buffer. `lib/libstink_surface.c` replaces the
file. A surface carries width, height and stride; every primitive
(fill, outline, line, text, blit) clips to it in 64-bit arithmetic
before touching memory, so the contract holds for any
arguments, not only for the ones today's callers pass.
`tests/test_surface.c` links the real file and makes 1.17 million calls
with coordinates and sizes that include `INT_MIN`, `INT_MAX` and every
value around each edge, on a surface between guard bands; one stray
byte fails it. The Rust side
(`libui::Surface`) draws only through these calls.

**G2 — windows redrawn wrongly. Fixed + retested.**
Nothing cleared what a moved, hidden or closed window left behind, and
when the last window closed nothing redrew at all. There is now one
writer to the screen. `kwm` composes into a back buffer and copies
damaged rectangles to video memory; a move, raise, resize, close or
exit damages the old and the new outline. `wm_test` compares the screen
with the first screenshot of the empty desktop after a drag, after
closing everything, after a full-screen program, after six open/close
cycles, after a trip to each screen edge and after a full house of
windows: zero differing pixels each time. Closing the last window
leaves the desktop, which is itself a window; ending the desktop
(Ctrl+Q) kills the other window owners and returns the screen to the
text menu, also tested.

**G3 — keyboard focus and event routing. Fixed + retested.**

| Question | Answer now | Checked by |
|---|---|---|
| Does the keyboard belong to the focused window? | Yes. Each window has its own key and raw-scan queues, filled only while it has focus; `SYS_GETKEY` from a window owner reads its own queue | `wm_test`: Esc closes exactly one of two windows |
| Can a background program take another's keys? | No. There is no shared queue while windows exist | same check: the unfocused window logged no exit |
| Are events lost? | A window's queue holds 63 events. Plain pointer motion replaces the last pending motion instead of queueing; if the queue is still full, the newest event is dropped (the compositor never waits for a program). A program that stops reading for long loses input | `apps_test`: every typed character reaches the editor and the disk |
| Order? | One queue per window, appended by one thread | by construction |
| Coherent mouse and keyboard contracts? | Both arrive as `win_event`; the pointer goes to the window under it, or to the window that received the press until every button is up | `apps_test` (calculator by mouse, a paint stroke) |
| Is focus valid after a process ends? | Focus moves to the topmost remaining window; all per-window state (grab, drag, resize, hover) is cleared in one place | `wm_test` full-house and `taskman` kill cases |
| Client-relative coordinates? | Yes, relative to the client area, which excludes the chrome | calculator buttons clicked by label position |
| Do drag and resize interfere with clicks? | They are states of the compositor entered only from the title bar and the grip; a press in the client area starts a grab for that client instead | `wm_test`, `apps_test` resize case |
| Press and release distinguished? | `WIN_EV_SCAN` carries make/break for programs that need it; `WIN_EV_KEY` is one event per character | Doom (full-screen path) turning stops on key-up |

Found while doing this: a program was never told that the pointer had
left its window, so the last button hovered stayed lit (`9c8c6d1`, with
a `wm_test` check that failed before).

**G4 — blocking desktop. Fixed + retested.**
The launcher did fork, exec and `waitpid`: one program at a time, with
the desktop frozen behind it. `rs-desktop` now starts a program and goes
back to its event loop; it collects children with the non-blocking
`SYS_REAP` (96) and sleeps in `SYS_WIN_WAIT` (97). `wm_test` runs seven
programs at once from it.

**G5 — window memory mapping. Fixed + retested.**
`paging_map_win_buf` skipped pages whose page table could not be
allocated and reported success. Now:

| Concern | Behaviour | Checked by (`ktest`) |
|---|---|---|
| Allocation failure, partial init | create and resize are all-or-nothing: frames, page tables and mappings are acquired first and rolled back together | by reading; the failure itself is not injected by a test. `win-limits` covers refused arguments |
| Frame ownership | the window owns its frames; the process only has `PG_SHARED` mappings, which address-space teardown drops without freeing | `win-leak`: 16 lifetimes with resizes, with and without an explicit destroy, frame count returns |
| Double free / use after free | destroy unmaps before freeing; a write to a destroyed window's buffer faults | `win-unmap` |
| Old buffer after resize | unmapped; a failed resize keeps the old buffer | `win-unmap`, `win-limits` |
| Fork | the child gets neither the window nor a view of its pixels | `win-unmap` (third part) |
| Exec, exit, kill | all release the window | `win-leak`, `win-exec-kill` |
| Sharing between processes | none: a buffer is mapped only into its owner | `win-unmap` |
| Limits | sizes, flags, a second window and bad pointers are refused; nine processes compete for eight slots without damage | `win-limits`, and `wm_test` end to end |

**G6 — compositor in the timer interrupt. Fixed + retested; measured.**
Composition moved to a kernel thread that interrupt handlers only wake.
Interrupt latency is therefore unaffected by drawing. What the thread
costs other processes is bounded by its longest frame, because the
kernel does not preempt itself. Section 3.4 has the numbers.

**G7 — silent failures in userland (`libui`). Fixed + retested.**
`libui` was rewritten around four types. `Window::open` returns `Option`
(the old constructor handed out a buffer whatever the kernel answered);
`resize` returns `bool` and keeps the old size on failure; file calls
return what the kernel returned, and each program shows a message when
one fails. Strings crossing the FFI are byte slices checked for a NUL
terminator; lengths are bounded before indexing; `static mut` state
became locals (the paint program keeps two large static buffers, each
borrowed once). The `sys_getpid` shim called syscall 43 instead of 45.
A Rust panic logs a line on serial and exits the process with code 1;
the system carries on. `wm_test` runs a program that
cannot get a window: it says so and ends.

### 3.3 Interaction matrix

| Interaction | Label | By |
|---|---|---|
| Open, close, open/close repeatedly | Verified | `wm_test` |
| Move in every direction, partly off the screen | Verified | `wm_test` (drag; four edges) |
| Resize by the corner, content intact | Verified | `apps_test` (editor) |
| Resize repeatedly, resize while the program animates | Verified | `apps_test` (`rs-vec`, four sizes while it animates) |
| Switch focus, overlap, change stacking, taskbar | Verified | `wm_test` |
| Keys with windows in different states | Verified | `wm_test` (focused / unfocused) |
| Buttons and fields | Verified | `apps_test` |
| Fast pointer movement | Verified in part | harness moves in 100-pixel steps; no human-speed test |
| Redraw of content | Verified | `apps_test` reads the text back after each action |
| End a program during interaction, and after a fault | Verified | `apps_test` (task manager kill), `ktest win-exec-kill`. A faulting owner: `ktest win-unmap` has ten window owners die of a fault and finds every frame and slot returned |
| Many processes | Verified | `wm_test` (seven windows), `ktest fair3` |
| Exhaust window slots and memory | Verified | `wm_test`, `ktest win-limits`, `ktest oom` |
| Return to desktop and to the menu | Verified | `wm_test`, `doom_test` |
| Ghosting, residue, windows that outlive their owner | Verified absent | pixel comparison with the empty desktop at eight points |
| Tearing | By design | no vertical-sync: a frame is copied to video memory when ready |
| Flicker | Verified absent by construction | the screen is only written from a finished back buffer |
| Leaks over open/close cycles | Verified | `ktest win-leak`, `leak-exec` |

### 3.4 What the compositor costs

Method: `kernel/sys/win.c` counts frames, pixels recomposed, pixels
copied and time (TSC calibrated against the PIT) and prints one line
per second in which it drew. `tools/qa/wm_perf.py` puts the desktop in
five situations and reads the lines. Host: the machine in the
environment table; one run each; times vary by ten to twenty percent
between runs.

| Situation | Frames | TCG, per frame | KVM, per frame |
|---|---|---|---|
| Still screen, with or without windows | none | 0 | 0 |
| Pointer moving | per input | 0.16 ms (nothing recomposed) | 0.02 ms |
| Dragging a 304x348 window | per input | 4.4 ms (10.8 ms worst) | 0.47 ms (0.96 ms worst) |
| A 480x400 window animating at 25 fps | 25/s | 4.4 ms: 11 % of the CPU | 0.52 ms: 1 % |
| Repaint of the whole screen | 1 | 5 to 25 ms | 1.6 to 3.5 ms |

The animation figures were 9.3 ms and 1.5 ms before `ff4b948`, which
stopped painting windows hidden under an opaque one and moved rows with
`rep movs`.

What this means for the rest of the system:

- **Interrupts** are never held off by drawing.
- **Scheduling**: a process that becomes runnable waits at most one
  frame for the compositor: 25 ms in the worst case measured under TCG.
  `ktest fair3` (three busy processes sharing the CPU evenly while the
  thread runs) passes.
- **Audio** is refilled from the sound card's interrupt and does not
  wait for the thread. **Network** reception waits for `knet`, which
  has the same priority as `kwm`: up to one frame of extra latency.
  Neither was measured under graphics load: **Not tested**.
- **Memory**: one back buffer the size of the screen (3 MiB at
  1024x768) plus each window's pixels (4 bytes per pixel).
- **Known cost left in**: damage is one bounding rectangle, so two
  small changes far apart repaint everything between them; and
  `SYS_WIN_FLUSH` takes no rectangle, so a program that changes one
  button still has its whole window copied. Both are **Future**.

### 3.5 Look

Judged by eye on the `--shots` output of the suites, not measured. One
palette and one set of metrics live in `libui` (background, raised
surface, selection, accent, dim text; 12 px padding, 8 px gaps, 20 px
rows, 8x16 font) and every program uses them; chrome is the kernel's, so title
bars, close boxes and focus colour are identical everywhere; controls
that did nothing and key hints that were not true were removed; every
window has a status line with the keys that work.

### 3.6 Independent review

When the work above was done, the window system, the shared-mapping
code and the new disk code were given to a reviewer that had seen none
of it and none of this log, with the scheduling rule as its only
briefing. It reported seven findings; each was checked against the code
before anything was changed.

| # | Finding | Status |
|---|---|---|
| R1 | `SYS_CLIP_READ` validated its destination for reading and then wrote to it: a read into the caller's `.text` faulted in ring 0 and halted the machine | **Fixed + retested** (`ktest text-kwrite`: `kernel exception 14 ... halted` before) |
| R2 | `SYS_SUSPEND`, `SYS_KILL`, `SYS_RAISE` and `SYS_SETPRIO` accepted the PID of a kernel thread: one call froze the screen and all input, or the network; a process could also take a priority ahead of the threads | **Fixed + retested** (`ktest kthread-guard`: `suspend(2) was accepted` before). On the kernel branch, since the network thread has the same exposure |
| R3 | ending the session freed window frames still mapped in owners that were killed but not yet gone | Fixed: such windows are hidden and destroyed when the owner exits. The session-end path is exercised by `wm_test`; the narrow race itself (an owner blocked in a pipe read into its window buffer) is not reproduced by a test |
| R4 | frame counts unchecked against the frame table above 1024x768 | Fixed; not reachable in the modes the bootloader selects, no test |
| R5 | the first window of a session was composed over an uninitialised back buffer | **Fixed + retested** (`wm_test`, lone window: 57344 stray pixels before) |
| R6 | `sys_win_show` on a visible window left its old outline on screen | Fixed by reading; no bundled program does this, no test |
| R7 | a button already held when the compositor got the screen back counted as a press | Fixed by reading; no test |

The reviewer found nothing in the shared-mapping code or in the disk
runs. It also confirmed by reading what several tests assert: bounds of
every blit, slot state reset on destroy, resize rollback, and that only
a wake-up is done from interrupt context.

One more defect surfaced while fixing R2. The kernel's build rule
listed no headers, so adding a field to `struct proc` rebuilt the files
that were edited and linked them with objects compiled for the old
layout; three unrelated `ktest` cases hung. Kernel objects are now
built with `-MMD -MP` (`83ca4ba`). Every result in section 6 is from a
clean build.

## 4. Applications

Each was built, opened on the booted system and used for what it is
for; `apps_test` does that on every run (39 checks) and reads the
results off the screen and off the disk. Every program ends on its quit
key and on the close box (same code path; the close box is exercised by
`wm_test` on one of them). Behaviour changed while porting:

| Program | What was wrong | Now | Label |
|---|---|---|---|
| all fourteen | a key equal to the previous one was dropped ("11" typed as "1"); the "F11 maximise" tested the scan code of the letter W; 60 Hz polling loops | events from the window queue; resizable windows where the layout adapts; sleep until an event or a timeout | Fixed + retested (`apps_test` types 11 * 11) |
| `rs-calc` | (shared defects above) | pocket-calculator semantics spelled out: left to right, repeated `=`, percentage | Fixed + retested for arithmetic by keyboard and mouse; the trigonometric keys are not covered |
| `rs-edit` | a file over 4096 bytes was loaded truncated and saved back shorter; save and read errors ignored; a new file always overwrote `NOTES.TXT`; only the last 128 bytes of a paste could be undone | oversized files refused; errors shown; first unused `NOTESn.TXT`; paste is one undo step | Fixed + retested (type, save, bytes on disk, resize); undo, paste and the refusal of large files are not covered by a test |
| `rs-files` | the scrollbar did nothing; launched the editor and waited for it | scrolls by keys and by dragging the bar; starts programs and the editor without waiting | Fixed + retested (list, End, open in the editor); dragging the scrollbar is not covered by a test |
| `rs-paint` | saved raw window memory (2.8 MiB) and ignored the result; line tool always 1 px | own canvas, 384 kB file with a header, checked on load; line tool at brush size with a preview. Old saves are not readable | Fixed + retested (save, bytes on disk, reload after reboot) |
| `rs-todo` | changes reached the disk only on S; the "prompt on exit" in its help did not exist | written after every change; failure shown until a write succeeds | Fixed + retested (reboot) |
| `rs-hex` | 64-file cap, size shown was bytes read | re-reads the directory; real size; says when a file is cut at 64 kB | Verified (first file); the 64 kB cap is a limit |
| `rs-sysinfo`, `rs-net`, `rs-taskman` | uptime divided ticks by 1000 (they are 10 ms); `ps` data was empty (K16); every program was named `kinit`; kill result ignored; refresh every 20 s instead of 2 | correct units; names from exec; kill reports failure; refuses PID 1 and itself | Fixed + retested |
| `rs-net` | showed the ARP cache next to unrelated counters | link, MAC, DHCP state, address, mask, gateway, DNS, then ARP | Fixed + retested |
| `rs-settings` | a "Mute" switch that only silenced the program's own test beep | removed; the layout switch shows what the kernel uses | Fixed + retested |
| `rs-clock`, `rs-cal`, `rs-about` | drew their own frames; redrew continuously | wake when what they show changes | Verified |
| `rs-vec` | its sine was off by 0.007 near ±π | argument folded into [−π/2, π/2]; worst error 3.5e-6 against the host's `sin` | Fixed (checked on the host); animation and resizing Verified |
| `rs-desktop` | section 3.2 G4 | | Fixed + retested |
| `rs-life` | made a full-screen window to draw through the redirect shims | a full-screen program again | Verified (`smoke-rs-life`) |

Known and left:

- **Future:** the editor quits without asking when there are unsaved
  changes (it says `[modified]`), cannot insert a Tab and has no mouse
  support.
- **Future:** the file browser tells the editor which file to open
  through a file, `LAUNCH.TMP`. An editor started from the desktop in
  the same instant could take the name. The browser removes the file
  if the editor never read it (`apps_test` checks none is left).
- **By design:** one window per process; no dialogs.

The console programs (shell, the C editor, the games, `stink-pkg`, the
demos) are covered by `test-headless` and the `smoke-*` scripts, which
pass unchanged in behaviour. From the desktop they run full screen.
The installer had no test; section 5.

## 5. Storage, installer, Doom

**Disk runs (A27). Fixed + retested.** Running the suites under KVM
showed Doom taking 115 s to reach its first frame, against 7 s under
TCG. StinkFS issued one ATA command per sector, and under KVM each port
access is a VM exit. Whole sectors now move in runs of up to 128 per
command (`cd2f22a`). Doom starts in 12 s under KVM. `ktest fs-runs`
checks the data through every path that changed, and did not finish in
60 s under KVM before the change. What remains slow there is the
driver polling the status port (A29): a 384 kB save takes 0.16 s under
TCG and about 4 s under KVM.

**Installer. Verified.** `install_test` runs it from the text menu onto
a blank second disk, reads the partition table and the file list of the
target from the host, and boots the target alone to the desktop.

**Doom.** Audited through the port layer (`doomgeneric_stink.c`, the
`doom-shims`, `libstink_stdio/alloc/posix`); the engine source is
unchanged.

| Aspect | Finding | Label |
|---|---|---|
| Start-up, WAD loading | works from the desktop tile and the menu; `FREEDOOM1.WAD` (28 MiB) read through StinkFS | Verified (`doom_test`). `DOOM2` and `FREEDM` were started from the text menu by a one-off script: each loads its own WAD and draws its title; not part of a suite |
| Rendering, scale | 640x400 centred on the 1024x768 screen; at 24 and at 32 bpp | Verified (32 bpp by `doom_test`; 24 bpp by one run on `-vga cirrus` with the screenshot read by eye). The optional 1024x640 upscale (`stinkdoom_fullscreen=1` in `STINK.CONF`) is Not tested |
| Timing | game tics from the PIT at 100 Hz, so 35 Hz tics land on 10 ms steps (jitter of one step, no drift) | By design |
| Speed | `-timedemo demo1`: 7117 tics in 1577 real tics, about 158 frames per second under TCG | Verified (measured once) |
| Keyboard | key-down and key-up reach the game (turning stops on release); letters were not mapped at all except Y and N, so a save could only be named with digits | Fixed + retested |
| Mouse | relative motion turns the view | Verified (`doom_test`); buttons Not tested |
| Sound effects | play through the SB16 mixer; the mixer's defects (A1, A18-A21) were kernel-side | mixer: Fixed + retested (`ktest audio-*`). That Doom's effects are audible and correct: Not tested (the suites run without an audio back end) |
| Music | not implemented (needs an OPL or MIDI synthesiser) | By design |
| Saving | failed: `./.savegame/doomsav0.dsg` does not fit a 15-character flat name, and each byte was its own syscall and sector rewrite | Fixed + retested (save, quit, restart, load; file checked from the host) |
| Exit | returns to an intact desktop; a second run in the same boot works | Verified |
| Long session | ten minutes of the attract loop under TCG: 118 of 119 screenshots taken 5 s apart differed from the one before; on quitting, the desktop was pixel-identical and the memory in use was what it had been (9 of 62 MiB); no kernel warning in the log | Verified (one run, not part of a suite). Hours of play: Not tested |
| In a window | not supported: Doom draws to the framebuffer and takes the whole screen. Running it in a window means a different video back end | By design |

## 6. Results

Final tree, same host. "KVM" is `STINKQA_KVM=1`.

| Suite | TCG | KVM |
|---|---|---|
| `make unittest` (66 host binaries) | 0 failures | n/a |
| `ktest` | 31/31 | 31/31 |
| `ktest --mem 24` (memory cases) | pass | pass |
| `qa-net`, `qa-input`, `qa-kbd` | pass | pass |
| `wm_test` | 41/41 | 41/41 |
| `wm_perf` | 6/6 | 6/6 |
| `apps_test` | 39/39 | 39/39 |
| `doom_test` | 13/13 | 13/13 |
| `install_test` | 8/8 (34 s) | 8/8 (260 s: the clone is where the polling driver, A29, costs most) |
| `test-headless`, eleven `smoke-*` | pass | not run (scripts do not use the harness) |

CI (GitHub Actions, TCG only) runs the first column on every push; its
state is on the pull requests, not copied here.

Static analysis: `make audit` with cppcheck 2.22 on the final tree
reports 17 findings outside the vendored Doom sources (the target did
not exclude them as its comment said; it does now). Each was read: none
is a defect. They are out-parameters filled in by a syscall that the
tool takes for uninitialised, copy loops bounded by a terminator it
does not follow, and a null data pointer that only ever comes with a
length of zero. clang-tidy and scan-build are not installed here:
**Not tested**.

## 7. PR #16: state and recommendation

`feat/userland-ui` keeps its original history; everything in this log
is on top of it, with the kernel work merged in from
`fix/kernel-recovery` (PR #17). Nothing was force-pushed and `master`
is untouched.

Recommendation: merge PR #17 first (kernel only; the behaviour changes
programs can observe are listed in `CHANGELOG.md`), then PR #16. The second depends on
the first and contains it. The window syscalls 85-94 did not exist on
`master`, so nothing released breaks.

What speaks for merging: every defect above has a test that failed
before and passes now; the suites pass on two CPUs; the previous
programs still pass their own tests. What speaks for waiting: the open
items of section 8, of which the TCP ones (N5-N10) are the most likely
to be met in use.

## 8. Not stable, not tested, not done

- **TCP** (N5-N10): retransmission under new sequence numbers, option
  bytes delivered as data, a listener freed by one RST, zero-window
  stall, FIN handling, DHCP accepting options in any state. Open.
  Simple request/response use works (`ktest net-ping`, DHCP, DNS).
- **N3** (socket ownership) is fixed by reading; no test.
- **A11**: stage 2 loads the kernel from the primary ATA master
  whatever the BIOS boot drive was. USB or second-disk boot of the
  loader is unsupported.
- **A24**: StinkFS names are case-sensitive except in the loader.
- **A28 / A29**: delete compacts with no recovery; the ATA driver polls.
- **Memory map**: RAM size from CMOS, not E820.
- **Long syscalls** hold everything (non-preemptive kernel).
- **Full-screen programs** cannot be left by a hotkey: a hung one holds
  the screen until the machine is reset.
- **Timer**: 10 ms granularity for sleeps and timeouts.
- **Serial log noise**: `cow: fault` and `fs: read@` lines for normal
  events.
- **Real hardware**: nothing here ran outside QEMU. KVM gives a real
  CPU, not real devices.
- **Legacy scripts** (`smoke-*`, `test-headless`) still use fixed
  sleeps; one of them failed once in CI for that reason and was changed
  to wait on its condition, the others were not.
