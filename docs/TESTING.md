# Testing

StinkOS is tested at five levels. They answer different questions, and
only the last four run the system's own code.

| Level | Command | What it runs | What it can prove |
|---|---|---|---|
| Host models | `make unittest` | `tests/test_*.c`, a copy of one piece of kernel logic per file | The arithmetic of that logic, as it was when the copy was made |
| Host tests on real sources | `make unittest` (`test_pmm`) | The kernel's own `.c` file linked into a host binary | That file's behaviour |
| Kernel cases in QEMU | `make ktest` | `apps/ktest.c` in ring 3 against the booted kernel, one VM per case | Kernel invariants end to end |
| Scenarios in QEMU | `make test-headless`, `make smoke-*`, `make qa-net`, `make qa-input`, `make qa-kbd` | Scripts driving a booted system through keyboard, mouse, serial and network | Whole-system behaviour |
| The desktop in QEMU | `make qa-wm`, `make qa-apps`, `make qa-doom` | The graphical session driven with pointer and keys, checked on screenshots, the serial log and the disk image | What a person at the machine would see |

`make qa` runs every suite that boots the system.

## Host models, and their limit

Most files in `tests/` do **not** link kernel code. Each one replicates a
kernel decision (same loop bodies, constants and branches) and drives it
with deterministic inputs. That catches off-by-one and wrap-around
mistakes in the logic cheaply, with nothing but the host compiler.

It does not catch the kernel drifting away from the copy. A model keeps
passing when the kernel function it was copied from is rewritten, and it
says nothing about how that function interacts with interrupts, paging or
other processes. Before the 2026-11 recovery pass, `make unittest` was
green while 13 of the first 14 `ktest` cases failed against the same
tree. Treat a model as a calculator check, not as evidence that the
kernel behaves.

When you change kernel logic on purpose, update its model in the same
commit. When you can, prefer the next level instead.

## Host tests on real sources

A kernel file that only does arithmetic (no privileged instructions, no
direct-map dereferences) can be compiled for the host as it is.
`tests/test_pmm.c` links `kernel/arch/pmm.c` this way, with two serial
stubs. New host tests should take this form whenever the file allows it.

Two userland libraries are tested the same way, from their real sources:

* `tests/test_surface.c` links `lib/libstink_surface.c` (the drawing
  code every window uses) and throws over a million calls with hostile
  coordinates and sizes at a surface surrounded by guard bytes. Any
  write outside the pixel buffer fails the test.
* `tests/test_stdio.c` links `lib/libstink_stdio.c` (buffered files, used
  by Doom) over an in-memory file system that counts syscalls: mixed
  reads, writes and seeks must produce the same bytes as a reference
  model, and a byte-at-a-time writer must not turn into one syscall per
  byte.

## Kernel cases in QEMU (`make ktest`)

`apps/ktest.c` is a ring-3 program with one function per kernel
invariant: no frame leaks across fork / exec / kill, a program break per
process, COW that holds when the kernel writes, pointer validation that
matches what is mapped, a scheduler that neither starves nor wastes CPU,
owned pipes and sockets, audio that survives its buffer going away.

`tools/qa/ktest.py` runs each case in its own VM:

1. copy `os.bin` (the build output is never written to);
2. add two files to the copy: `AUTORUN` (the kernel menu starts the app
   it names at boot) and `KTEST.ARG` (the case name);
3. boot it with a NIC and an SB16 attached, wait for QEMU to exit
   (the app powers the machine off when done), read the serial log.

A case reports `PASS`, `FAIL <why>`, `CRASH` (kernel exception in the
log) or `HANG` (no verdict before the timeout). One VM per case means a
case that takes the kernel down does not hide the rest.

```sh
python3 tools/qa/ktest.py                  # all cases
python3 tools/qa/ktest.py leak-exec -v     # one case, with the serial log
python3 tools/qa/ktest.py --mem 512        # same, with 512 MiB of RAM
```

To add a case: write `static int t_name(void)` in `apps/ktest.c`
returning 0 or `FAIL("why")`, add it to `cases[]` and to `CASES` in
`tools/qa/ktest.py`. Write it so that it fails on the bug: run it against
the unfixed kernel first.

## Scenarios in QEMU

`tools/qa/stinkqa.py` is the shared driver: QMP keyboard and mouse
input, the serial log, screenshots, and helpers to add, read or remove
files in a private image copy. Scripts built on it wait on serial output,
on the screen changing or on QEMU exiting rather than on fixed sleeps.
Every wait has a timeout; `STINKQA_TIMEOUT_SCALE=4` multiplies them all
for slow hosts (CI sets it).

| Script | Scenario |
|---|---|
| `tools/qa/net_rst.py` | the host opens TCP connections to a closed guest port while an app runs; the guest must reset them and its kernel stack audit must stay quiet |
| `tools/qa/input_flush.py` | keys typed at an app that never reads them must not be replayed into the menu when it exits |
| `tools/qa/kbd_test.py` | modifier state key by key: two Shifts, a held Caps Lock, the Pause sequence |

`tools/test-headless.py` and `tools/smoke-*.py` are older scripts that
boot into the kernel's text menu and check the serial log. They take
their image from `stinkqa.menu_image()` (a private copy without the
`AUTORUN` file, so the desktop does not start) and use fixed sleeps; do
not run two of them at once.

## The desktop in QEMU

These three suites need Python's Pillow for screenshots, and boot the
stock image, which starts the desktop.

They do not guess where things are. The window manager logs every state
change on the serial line (`win: show pid=5 x=.. y=.. w=.. h=..`,
`moved`, `resize`, `focus`, `destroy`, `fullscreen`), and the desktop
logs where each tile is, so a script clicks "the close box of the
window owned by pid 5" from what the system reported. What the user
would see is then read back from a screenshot: `find_text` /
`read_text` match the kernel's own 8x16 font against the pixels, so a
test can assert that the calculator's display shows `56`.

| Suite | What it does |
|---|---|
| `tools/qa/wm_test.py` | One session: start programs from the desktop, focus, raise, drag (including off every screen edge), resize, close by the close box and by the taskbar, kill a window's owner, eight windows at once, full-screen takeover and return, leave to the text menu and come back. After each step it checks the screen for leftovers: a moved or closed window must leave exactly the pixels that were under it |
| `tools/qa/apps_test.py` | Every windowed program in its own VM: opens it, uses it for what it is for (a sum in the calculator, typing and saving in the editor, a stroke in the paint program, killing a process from the task manager, ...), checks the result on screen, then checks what reached the disk by reading the image from the host, and for programs that save, boots a second time to see the data come back |
| `tools/qa/doom_test.py` | Doom from the desktop tile: title screen, new game through the menus, walking, turning and firing with the HUD ammo count checked, a save and a load, sound effects reaching the mixer, quit back to the desktop with the screen restored, and a second run in the same boot |

A failure saves the last screenshot and the serial log under the
directory the script prints.

To add a case for a new program, add a function to `apps_test.py`: it
gets a booted desktop, calls `launch("rs-name")` (which returns the
window's geometry as the kernel reported it), sends input in client
coordinates and asserts on `read_text` / the disk.

## Adding a host model

1. Pick one kernel decision worth pinning: off-by-one risk, unsigned
   wrap-around, a contract documented in a comment but enforced nowhere.
2. Create `tests/test_<area>.c`. The top comment must say which kernel
   file it models, and which QEMU case (if any) covers the real code.
3. Add a build rule and the two `unittest` entries in `Makefile`.

## When a test fails

Two cases:

- **You did not mean to change the behaviour.** A bug. Fix the kernel.
- **You changed it on purpose.** Update the mirror in the same commit
  as the kernel change. The commit message should make that explicit
  (`refactor: simplify foo, update test_foo`).

Never silence a failing test by deleting an assertion. Either the
assertion is wrong (update it) or the kernel is wrong (fix it).

## What's covered today

Networking lives in `tests/test_arp_*`, `test_dhcp_*`, `test_dns_*`,
`test_ipv4_*`, `test_tcp_*`, `test_udp_*`, `test_icmp_*`,
`test_eth_*`. Audio in `test_mixer`, `test_audio_mode`. Filesystem in
`test_stinkfs_dir`, `test_fs_grow`, `test_fs_delete`, `test_mbr_*`.
Process / memory in `test_pmm`, `test_mmap_overflow`, `test_pipe`,
`test_timer`, `test_sched`. Drivers in `test_rtc_alarm`,
`test_blit_*`, `test_fb_rect_clip`, `test_utf8_collapse`. Kernel
state in `test_klog`. Userland libraries in `test_surface`,
`test_stdio`.

For an authoritative list run `make help` and look at the `unittest`
target, or read the `unittest:` prerequisite list in `Makefile`.

## Reproducible QEMU runs: always pass `-snapshot`

StinkOS writes back to its disk during normal operation -- shell command
history (`SHELL.HIS`), game high scores, `STINK.CONF` edits, and the
StinkFS directory itself whenever a file grows or is created. QEMU's
default raw-drive mode propagates those writes straight back to
`os.bin`, so two consecutive boots see different state. The second boot
loads a half-modified disk, the menu can come up empty, headless tests
go non-deterministic, screenshots get corrupted.

`-snapshot` redirects every guest write to a temporary file that QEMU
discards on exit. `os.bin` stays bit-identical to what `make all`
produced; every run starts from the same clean image.

When you need it:

| Command | -snapshot? | Why |
|---|---|---|
| `make run` | NO | Interactive use -- you want history + saves to persist |
| `make run-install` | NO | Installer must write the target disk |
| `make run-installed` | NO | Interactive use of the installed system |
| `make run-iso` | NO | Same |
| `make test-headless`, `make smoke-*` | YES (already set in the scripts) | CI must be deterministic |
| `make ktest`, `make qa-*` | not needed: each run boots a private copy of the image | Tests add files and reboot on the copy; `os.bin` is opened read-only |
| `tools/capture-screens.py` | YES (set explicitly in the script) | Screenshots reproducible across reruns |
| Any ad-hoc debug rerun where you want a clean boot | YES | Bypass disk corruption from prior runs |

Rule of thumb: if the QEMU run is supposed to be reproducible across
invocations, pass `-snapshot`. The flag is one line, the bug it
prevents costs hours to track down (the symptoms look like kernel
regressions, not capture-tooling artefacts).
