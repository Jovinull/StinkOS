# Testing

StinkOS is tested at four levels. They answer different questions, and
only the last three run the kernel's own code.

| Level | Command | What it runs | What it can prove |
|---|---|---|---|
| Host models | `make unittest` | `tests/test_*.c`, a copy of one piece of kernel logic per file | The arithmetic of that logic, as it was when the copy was made |
| Host tests on real sources | `make unittest` (`test_pmm`) | The kernel's own `.c` file linked into a host binary | That file's behaviour |
| Kernel cases in QEMU | `make ktest` | `apps/ktest.c` in ring 3 against the booted kernel, one VM per case | Kernel invariants end to end |
| Scenarios in QEMU | `make test-headless`, `make smoke-*`, `make qa-net`, `make qa-input` | Scripts driving a booted system through keyboard, mouse, serial and network | Whole-system behaviour |

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
input, the serial log, PNG screenshots, and helpers to add or remove
files in a private image copy. Scripts built on it wait on serial output
or on QEMU exiting rather than on fixed sleeps.

| Script | Scenario |
|---|---|
| `tools/qa/net_rst.py` | the host opens TCP connections to a closed guest port while an app runs; the guest must reset them and its kernel stack audit must stay quiet |
| `tools/qa/input_flush.py` | keys typed at an app that never reads them must not be replayed into the menu when it exits |

The older `tools/test-headless.py` and `tools/smoke-*.py` predate the
driver and use fixed paths under `/tmp` and fixed sleeps; do not run two
of them at once.

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
state in `test_klog`.

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
| `make test-headless` | YES (already set in `tools/test-headless.py`) | CI must be deterministic |
| `tools/capture-screens.py` | YES (set explicitly in the script) | Screenshots reproducible across reruns |
| Any ad-hoc debug rerun where you want a clean boot | YES | Bypass disk corruption from prior runs |

Rule of thumb: if the QEMU run is supposed to be reproducible across
invocations, pass `-snapshot`. The flag is one line, the bug it
prevents costs hours to track down (the symptoms look like kernel
regressions, not capture-tooling artefacts).
