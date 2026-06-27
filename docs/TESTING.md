# Testing

StinkOS runs a host-side regression suite via `make unittest`. The suite
exists because the kernel ships exclusively into QEMU (no real hardware
in CI, no real users hammering it), so silent regressions in sub-byte
arithmetic, tick wraparounds, RFC corner cases, and ring-buffer seams
would otherwise sit unnoticed until someone manually replicated the
exact failing scenario.

## How the tests work

The suite is **not** linked against kernel code. Each test in `tests/`
is a self-contained C file that:

1. Replicates the kernel's decision logic byte for byte (the same loop
   bodies, the same constants, the same branches).
2. Drives it with deterministic inputs the host compiler can run.
3. Exits 0 on success, 1 on failure.

The Makefile compiles each test with the host `$(HOST_CC)` and runs
the produced binary; the first failure exits the suite.

This means **the tests are mirrors, not bindings.** When you change
the kernel logic on purpose, you update the mirror in the same commit.
If you forget, the next person running `make unittest` discovers the
divergence and either updates the mirror to match the intentional
change, or reverts the kernel back to the contract the test pinned.

## Why this layout

We considered three alternatives and rejected each:

| Approach | Why not |
|---|---|
| Cross-compile kernel objects with a stub loader, link tests against them | Forces a second build pipeline and host-vs-target ABI gymnastics for a hobby OS that already has a tight build. The mirror approach catches the same regressions with `gcc tests/foo.c -o foo`. |
| Boot QEMU and probe via serial | Slow, hard to make deterministic for protocol corner cases (no programmable lossy network in unit-test scope). Acceptable for end-to-end smoke; not for the ~50 invariants we pin. |
| No host tests; review only | The bugs we pin (TCP wraparound, fb_rect overflow loop, mmap size overflow, klog seam, fragment overlap) are exactly the ones that survive review every time. |

## Adding a new test

1. Pick one kernel decision worth pinning. Good candidates: anything
   with off-by-one risk, anything with unsigned wraparound, anything
   whose contract is documented in a comment but enforced nowhere.
2. Create `tests/test_<area>.c`. Top comment must say which kernel
   file/lines you mirror and why a regression there would be silent.
3. Replicate the logic with the same constants and branch structure.
   Resist the urge to "improve" it; the goal is to detect drift.
4. Add a build rule and two unittest list entries in `Makefile`:
   - the `$(TEST_BIN)/test_<area>` rule
   - the prerequisite list at the top of `unittest:`
   - the runner line in the body of `unittest:`
5. Run `gcc -O2 -Wall -Wextra -o /tmp/<random> tests/test_<area>.c &&
   /tmp/<random>` and confirm `0 failure(s)`.

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
