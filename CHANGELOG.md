# StinkOS changelog

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Version numbers follow [SemVer 2.0](https://semver.org/spec/v2.0.0.html);
for a hobby OS, "major" means a wire-incompatible kernel ABI break.

## [Unreleased]

## [0.8.0] -- VFS multi-mount

`kernel/fs/fs.c` is now routed through a 2-slot mount table behind an
`fs_ops` dispatch. Filenames may carry an optional DOS-style 2-char
prefix (`A:foo` / `B:foo`) that picks the mount slot; no prefix
defaults to slot 0 (the boot mount). A new `SYS_MOUNT` syscall lets
userland register an additional StinkFS region at runtime, so a second
disk image can be wired up and accessed under `B:` without touching
the primary mount.

The bundled backend stays StinkFS; the ops indirection is zero-overhead
today (single static dispatch) and lets a future DevFS / FATFS / ProcFS
plug in with a new ops table + one mount call -- no plumbing rewrite.

### Added

- `struct fs_mount` + `struct fs_ops` in `kernel/fs/fs.c`; per-slot dir
  buffer, IO bounce, LBA bounds, ATA drive index, ops table
- `stinkfs_ops` registers all existing fs.c logic behind the ops table;
  slot 0 (`A:`) auto-registers at `fs_init` with the Makefile-pinned
  primary location on ATA drive 0
- `fs_mount_register(slot, drive, dir_lba, data_lba, data_end)` API in
  `kernel/fs/fs.h`
- DOS-style prefix resolver inside the fs public API: `A:..`/`B:..`
  (case-insensitive) routes to the slot; missing prefix = slot 0
- `SYS_MOUNT` syscall (84): `ebx=(slot<<8)|drive`, `ecx=dir_lba`,
  `edx=data_lba`, `esi=data_end` -> 0 / -1
- `sys_mount(slot, drive, dir_lba, data_lba, data_end)` libstink wrapper
- `apps/mounttest.c` end-to-end validator: registers slot 1 at unused
  disk LBAs, writes `B:hello` with a marker, reads back, asserts
  byte-equal, deletes, asserts read-after-delete fails
- `tools/smoke-vfs-mounts.py` + `make smoke-vfs-mounts` target

### Notes

- `:` is now a reserved character in filenames (interpreted as the
  prefix separator). No existing apps use it.
- Slot 0 (`A:`) is the boot mount and cannot be re-mounted at runtime.
- Mount table is volatile -- mounts evaporate on reboot. A future
  release can persist `MOUNT.CFG` to slot 0.

## [0.7.0] -- COW fork

`paging_copy_user_pgdir` no longer eagerly duplicates every user frame.
At fork time, parent and child share frames; writable pages are tagged
PG_COW (PTE software bit 9) and have their RW bit cleared on BOTH
sides. The first writer takes a #PF which a new
`paging_handle_cow_fault` resolves by either restoring RW (last owner)
or allocating + copying into a fresh frame (the writer's PTE then
points at the private copy while the original sticks with the other
owner). Read-only pages (.text / .rodata under v0.5 W^X) stay shared
without PG_COW -- writes to those remain real W^X violations and the
trap path kills the offender as before.

Cuts fork cost from "memcpy every present user frame" to "flip RW +
ref_inc". Typical apps that fork then exec touch only a handful of
pages before the exec replaces everything, so most copies that today's
eager path performs never have to happen at all.

PMM gains a per-frame u8 refcount (`pmm_ref_inc`, `pmm_free` now
ref-aware, `pmm_ref` for diagnostics). Single-CPU only -- when SMP
lands, refcount + COW need atomic ops + per-CPU lock.

### Added

- `pmm_ref_inc(frame)` / `pmm_ref(frame)` and ref-aware `pmm_free` in
  `kernel/arch/pmm.{c,h}`; PMM tracks 8192-frame refcount array (8 KiB
  static; covers the full 32 MiB PMM range)
- `PG_COW` (PTE bit 9, OS-available per Intel SDM Vol 3A §4.4) macro
  in `kernel/arch/memlayout.h`
- `paging_handle_cow_fault(va)` in `kernel/arch/paging.{c,h}` --
  refcount-aware fault resolver: refcount==1 shortcut (just lift RW)
  vs refcount>1 (alloc + memcpy via direct map + install RW at writer's
  PTE + ref_dec old frame)
- `paging_copy_user_pgdir` rewritten: shares frames, marks PG_COW on
  writable pages, calls `pmm_ref_inc` per shared frame, no contents
  memcpy
- `trap.c` #PF handler routes write+user+protection faults to
  `paging_handle_cow_fault` first; falls through to existing
  app-killer when the page lacks PG_COW (real W^X violation or
  non-COW fault)
- Boot serial line `cow: fault va=0x... ref=N` per COW fault
- `apps/cowtest.c` end-to-end validator: mmap a page, write
  PRE_MARKER, fork, parent + child write distinct markers, assert
  divergence
- `tools/smoke-cow.py` + `make smoke-cow` target
- `tests/test_pmm.c` extended with 8 refcount assertions (fresh alloc
  = 1, ref_inc bumps, multi-owner free drops to 0, double-free no-op,
  oob inputs ignored)

### Changed

- `pmm_alloc` now sets refcount=1 on every handout (was: no tracking).
  Behavior unchanged for callers that don't use `pmm_ref_inc` -- the
  alloc / free cycle still matches the v0.6 invariants.

## [0.6.0] -- ACPI static-table parsing

Section 7 ACPI lands: kernel locates the RSDP via the IA-PC scan
(EBDA + BIOS area), walks the RSDT (ACPI 1.0) or XSDT (2.0+), and
parses the two tables we actually need today -- FADT for soft-off
shutdown and MADT for CPU + IOAPIC topology. `sys_shutdown` now tries
real ACPI S5 first (FADT-described PM1a_CNT_BLK port write) and only
falls back to the legacy QEMU / Bochs / VirtualBox port-magic paths
when ACPI is absent. MADT reports CPU count + LAPIC base + IOAPIC
base on the boot serial line, ready for the future SMP bring-up.

No AML interpreter: SLP_TYPa is hardcoded to 5 (the value used by
essentially every PC firmware including QEMU). See TODO.md
"AML interpreter -- FUTURE" for the honest impact assessment on
laptop hardware and the path forward if we ever need it.

### Added

- `kernel/arch/acpi.{c,h}` with `acpi_init`, `acpi_available`,
  `acpi_find_table(sig4)`, `acpi_shutdown`, `acpi_cpu_count`,
  `acpi_lapic_base`, `acpi_ioapic_base`
- IA-PC RSDP scan: 16-byte aligned over EBDA (BDA word at phys
  0x40E) then 0xE0000..0xFFFFF, checksum-validated
- RSDT and XSDT walker, every found table checksum-validated and
  cached for `acpi_find_table` linear scan
- FADT (signature "FACP") parser exposing PM1a_CNT_BLK
- ACPI S5 shutdown path: writes `(SLP_TYPa<<10) | SLP_EN` to
  PM1a_CNT_BLK port, falls through to legacy port-0x604 / 0xB004 /
  0x4004 if ACPI is absent or refuses
- MADT (signature "APIC") walker iterating Local APIC (type 0) and
  IOAPIC (type 1) entries
- Boot serial log: `acpi: RSDP @ ...`, `acpi: RSDT entries=N`, per-
  table `acpi: <sig> @ <phys> len=<n>`, `acpi: PM1a_CNT_BLK port=...`,
  `acpi: cpus=N lapic=... ioapic=...`
- `bootdiag` row `firmware: acpi` (OK / ABSENT)

## [0.5.0] -- PAE + W^X

Section 7 of the roadmap: paging upgrades from 32-bit 2-level (4 MiB
PSE) to PAE 3-level (PDPT -> PD -> PT, 8-byte entries, NX at bit 63),
and W^X lands across kernel image + every user process. The kernel
image now refuses execution from its own .data / .bss and refuses
writes to .text / .rodata. Userland apps are linked with three program
headers so each segment's PTE permissions match the ELF p_flags:
.text R-X, .rodata R-NX, .data + .bss RW-NX. A new `wxattack` ELF
proves the gate fires: it stamps a one-byte shellcode into a global
and calls it via a function pointer; the CPU page-faults at the first
instruction fetch and the kernel kills the process.

### Added

- CPUID feature detect for PAE + NX (`kernel/arch/cpuid.{c,h}`) plus a
  boot-time serial feature summary
- `IA32_EFER.NXE` enabled in `kentry.s` under a CPUID guard (skipped on
  CPUs that don't advertise NX so the WRMSR can't fault)
- Bootstrap PAE paging in `kentry.s`: PDPT (4 entries) + 4 PDs of
  2 MiB PSE PDEs covering identity-low, identity-mid, the high-half
  KERNBASE mirror and the DEVSPACE MMIO window
- `PG_NX` + PAE flag macros in `memlayout.h`
- Per-proc PDPT in `paging.c`: PDPT[0] = own PD0 (user space), slots
  [1..3] = three shared kernel PDs across every proc
- `walk_user_pte` (xv6-riscv-style 3-level walk) underpins
  `paging_init_user_pgdir`, `paging_copy_user_pgdir`,
  `paging_destroy_user_pgdir`, and the new per-segment perm API
- Kernel W^X: PD2[0] split into a 4 KiB PT so .text gets R-X, .rodata
  gets R-NX and .data / .bss / stack get RW-NX. The rest of the
  direct map and DEVSPACE PSE PDEs carry NX so the kernel cannot jump
  into ordinary data frames
- Kernel section symbols (`__kernel_text_start/end`,
  `__kernel_rodata_start/end`, `__kernel_data_start/end`) in
  `boot/kernel.ld`
- User W^X via `paging_user_set_segment_perms(va, len, exec, write)`,
  called by `elf_load` after each PT_LOAD's bytes are in place
- Three-PHDR `apps/app.ld` (text R+X, rodata R-only, data R+W) so the
  loader has real `p_flags` to act on
- `apps/wxattack.c` adversarial ELF + `test-headless` assertion
  `wx attack blocked`
- `test-headless` now boots QEMU with `-cpu Penryn` so TCG advertises
  NX (without it the kernel skips `EFER.NXE` under its CPUID guard and
  W^X cannot be exercised)

### Changed

- `apps/game.s` writable globals moved from `.text` into `.section .data`
  so they don't trip the user W^X gate on the first write

## [0.4.0] -- multitasking proper + higher-half kernel

§1 of the roadmap closes: per-process page directory, `sys_fork`,
`sys_exec` upgrade. Kernel relocates to higher-half virt 0x80100000
(xv6-public pattern) so user PTs in `[USER_BASE, USER_END)` can never
shadow a kernel deref of a physical frame -- the bug class that bit the
fork landing is now impossible by construction.

### Added (post-rc1 -- the §1 finish line + higher-half migration)

- Per-process page directory (`paging_create_user_pgdir` /
  `paging_destroy_user_pgdir` / `paging_copy_user_pgdir`)
- CR3 swap in scheduler (`paging_switch` before every `context_switch`)
- `sys_exec` upgrade: new pgdir per exec, old pgdir destroyed on success
- `sys_fork` (SYS_FORK, syscall 83) -- eager copyuvm, no COW in v1
- Shell `bg <appname>` (fork + exec a child process in the background)
- Higher-half kernel link (virt `0x80100000`, LMA `0x100000` via `AT()`)
  -- xv6-public pattern; bootstrap pgdir in `kentry.s` does the
  identity-low -> high-half transition then drops identity-low
- `KERNBASE / KERNEL_DIRECT_MAP / KERNEL_DEVSPACE` in
  `kernel/arch/memlayout.h` + `P2V` / `V2P` macros
- `KVA()` helper in `paging.c`: every phys-frame deref routes through
  the high-half mirror -- safe under any active CR3
- Drivers migrated to `V2P()` for hardware DMA / descriptor addrs:
  audio SB16 buffer, e1000 RX/TX rings + descriptor base, ata Bus
  Master PRDT
- `make smoke-multiproc` -- fast (~25 s) standalone target asserting
  shell bg fork+exec
- `make test-headless` extended with a multi-proc fork+exec phase
- `tools/capture-screens.py` captures `shell_bg_demo.png` (shell prompt
  alive while a forked child runs in the background)
- TSS.esp0 follows the running proc's kstack on every context switch

### Fixed

- `app_return` for fork-child path: ZOMBIE + `proc_yield` instead of
  `menu_exit`'s longjmp (was crashing on PID > 1 exit)
- Stale `user_pts[]` PT cache after fork (now derived from active
  page_dir on every lookup)
- Kernel writes through the identity-low alias clobbering user
  `.rodata` during `paging_copy_user_pgdir` -- temporary fix via
  `kernel_only_pgdir` CR3 swap (commit `5b00964`), then permanent
  fix via the higher-half migration (drops the alias entirely)

### Original v0.4-rc1 contents

### Added

- N-chunk kernel loader (replaces the 2-stage fixed loader)
- PCB struct, process table and round-robin scheduler driven by PIT IRQ0
- `sys_getpid` / `sys_kill` / `sys_wait` / `sys_waitpid`
- Anonymous pipes with blocking read/write
- Cooperative signal model (SIGINT / SIGTERM)
- Per-process VFS descriptor table
- `sys_mmap` / `sys_munmap`
- Kernel preemption inside long syscalls (sti at dispatch entry)
- One-shot kernel timer subsystem
- SB16 half-buffer IRQs, per-channel sample-rate resampling, master volume
- Per-app suspend-silence groundwork (mixer channels tagged by owner PID)
- TCP retransmit + RTO, LISTEN passive open, congestion control (cwnd +
  slow start), OOO reassembly, window-scaling option parser, SACK emission
- IPv4 fragment reassembly (2-slot pool, 8 KiB max, TTL recycling)
- IPv4 routing through DHCP-supplied gateway
- IRQ-driven RX on e1000 (drains DD descriptors every IRQ)
- BSD-style userland socket API (`socket / bind / listen / accept /
  connect / send / recv / closesocket`)
- `SYS_NETSTAT` syscall + `netstat` shell command
- `SYS_MEMINFO` + `mem` shell command + throttled PMM OOM serial log
- `SYS_MBR_READ` syscall + installer-side target partition display
- `SYS_RTC_READ` syscall exposing CMOS wall clock to userland
- `SYS_SHUTDOWN` / `SYS_REBOOT` (QEMU shutdown port + 8042 reset fallback)
- `SYS_BLIT_SCALED` kernel-side nearest-neighbour scaler
- `SYS_KLOG_READ` syscall + 8 KiB kernel log ring + `dmesg` shell command
- `SYS_SUSPEND` / `SYS_RESUME` (also silences mixer channels of the
  suspended PID)
- `SYS_SETPRIO` / `SYS_GETPRIO` (scheduler priorities 0..31, default 16,
  lowest value wins with round-robin tie-break)
- `SYS_PROC_INFO` + `ps` shell command
- ICMP destination-unreachable reply for UDP packets on unbound ports
- DNS response cache (8 entries, 60 s TTL)
- TCP keepalive probes after configurable idle window
- Doom fullscreen mode (1024x640 letterbox via `SYS_BLIT_SCALED`) gated
  by `stinkdoom_fullscreen=1` in `STINK.CONF`
- Installer rewrites target boot sector explicitly after the clone
- `stink-pkg replay` re-installs entries from a `STINKPKG.LCK` snapshot
- `stink-pkg` no-deps install via uppercase `I` menu key
- Mixer resamples per channel via Q16.16 step from src_rate to output
  rate (`audio_mix_play_rate`)
- Host unit tests grown to sha256 + inet_addr + mixer + ipv4_checksum +
  tcp_options + sched (6 binaries through `make unittest`)
- `stink-pkg`: recursive dep resolver, `upgrade`, `query`, conflict
  detection, version pinning (`STINKPKG.PIN`), `STINKPKG.LCK`
  lockfile, configurable repo URL via `STINKPKG.CONF`
- `tools/make-stinkpkg.py` package builder + `tools/repo-server.py`
  reference HTTP repo
- Installer: ATA Bus Master DMA via PIIX BMIDE, custom install size,
  target MBR partition display, first-boot `FIRSTBOOT.RUN` marker
- `make stinkos-install.iso` distro image with MBR partition table
- `make sample-packages` bundles `edit/snake/pong/hello` as `.stinkpkg`
- `make unittest` host-side test suite (sha256, inet_addr, mixer,
  ipv4_checksum, tcp_options)
- `make audit` static-analysis sweep
- CI: builds + headless test on every push branch, release workflow
  publishes `stinkos-install.iso` on `v*` tags, GitHub Pages workflow
  deploys the sample stink-pkg repo
- Shell scrollback (256 lines, PgUp/PgDn) and persistent history via
  `SHELL.HIS`, hostname stored in `STINK.CONF`
- Doom: better SFX upsampling (linear interpolation), demo recording /
  playback via `DOOMARGS.CFG`, save-game via `rename()` copy-and-delete
- `docs/ARCHITECTURE.md` / `SYSCALLS.md` / `NETWORK.md` / `STINKFS.md` /
  `PACKAGING.md` / `MEMORY.md` / `TUTORIAL.md`, plus `apps/doom/COPYING`
  for the bundled Doom engine
- TCP fast retransmit (3 dup-ACK threshold), SACK sender-side scoreboard
  + retransmit clamp, zero-window persist probe, TIME_WAIT 2*MSL slot
  cleanup, RFC 5961 RST/SYN gates, RFC 793 SYN_RECEIVED ack mismatch RST,
  real advertised window from rx ring free space, TCP socket reap on PID
  death
- UDP / TCP / ICMP receive checksum verify; ICMP unreachable rate limit
  (RFC 1812 §4.3.2.8); ICMP echo Smurf mitigation (RFC 1122 §3.2.2.6);
  IPv4 martian source filter; LSRR/SSRR source-route drop; Teardrop
  overlapping-fragment guard
- DHCP DISCOVER/REQUEST retransmit, DNS query retransmit with optional
  fallback to a secondary server; gratuitous ARP after lease bind; ARP
  request rate limit + 60s cache entry TTL; DHCP `secs` field populated
- SB16 16-bit signed and stereo signed-16 playback paths (DMA channel
  5 + DSP commands 0xB6/0x30); `SYS_AUDIO_MODE` / `SYS_AUDIO_QUERY`;
  mixer channel silence on SYS_EXIT/SYS_KILL
- `SYS_MBR_WRITE` + installer writes a real MBR partition table after
  clone; `SYS_RTC_SET_ALARM` / `SYS_RTC_CLEAR_ALARM` /
  `SYS_RTC_ALARM_FIRED` via IRQ8
- Brazilian ABNT2 keyboard layout selectable via `STINK.CONF keymap=br`
- Shell `version` command; `make help` target; UTF-8 multi-byte
  sequences collapsed to `?` in the scrollback instead of garbling
- SYS_LOG / SYS_DRAWTEXT / SYS_DNS_REQUEST user-string bounded copies
  (kernel no longer deref's arbitrary user pointers); SYS_BLIT_SCALED
  width*height overflow guard; paging_user_mmap/munmap size overflow
  guard; SYS_SOCK_CONNECT / SYS_PING / SYS_SOCK_LISTEN dst+port
  validation; SYS_DISK_COPY 4096-sector cap; fb_rect pre-clip;
  Doom music stubs declared intentionally silent forever
- Host-side regression tests: Ethernet MTU, ARP storm + ARP TTL +
  ARP request rate limit, StinkFS dir, TCP dup-ACK / SACK use /
  persist / TIME_WAIT / checksum / RST gate / rx wnd / SYN gate /
  TCB lifecycle / close PID / wscale parser / OOO park-drain /
  cwnd + slow-start + RTO collapse / RTO doubling + cap +
  retry-limit / keepalive idle+interval, ICMP rate limit + echo
  reply, IPv4 unicast filter + source route + martian source +
  Teardrop overlap + 2-slot reasm pool alloc, DNS retry + DNS
  cache TTL+round-robin / DHCP retry + DHCP DNS2, RTC alarm,
  audio mode, MBR write, UTF-8 collapse, blit overflow +
  fb_blit_scaled nearest-neighbour + fb_rect pre-loop clip,
  PMM + paging_user_mmap/munmap overflow + paging_user_set_brk
  align/grow/shrink + range_ok span gates, anon pipe ring/EOF/
  EPIPE, klog ring pre/post-wrap, one-shot timer round-up + cb
  re-arm + tick wrap, fs_grow noop/inplace/relocate +
  fs_file_delete compaction, ELF32 loader 12-gate validation
  chain, BR ABNT2 keymap diffs vs US fallback, VFS fd table
  alloc/cursor/seek, proc_reap state gates +
  ZOMBIE-is-not-living invariant, PS/2 mouse packet decoder
- `docs/TESTING.md` describing the host-mirror unit test contract,
  how to add a new test, and what's covered today

### Changed

- Repo layout split into `kernel/{arch,drivers,fs,sys,ui}` + `boot/` + `lib/`
- `interrupts.c` split into `trap.c` (IDT/PIC/IRQ) + `syscall.c`
- App ELFs loaded from StinkFS by name (no fixed-LBA TOC)

## [0.3.0] -- pre-multitasking

First tagged release. Single ring-3 app at a time, full graphical menu,
StinkFS, e1000 + TCP/UDP stack, SB16 audio with software mixer, Doom1 /
Doom2 / FreeDM bundled, ATA installer, `stink-pkg` package manager
scaffold. See the `v0.3.0` git tag for the exact commit.
