# StinkOS changelog

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Version numbers follow [SemVer 2.0](https://semver.org/spec/v2.0.0.html);
for a hobby OS, "major" means a wire-incompatible kernel ABI break.

## [Unreleased]

Work in progress on `feat/v0.4-multitasking-loader`. See `TODO.md` (local,
untracked) for the full pending list.

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
  `PACKAGING.md` / `MEMORY.md` / `TUTORIAL.md`, plus `COPYING.GPL`
  for the bundled Doom engine

### Changed

- Repo layout split into `kernel/{arch,drivers,fs,sys,ui}` + `boot/` + `lib/`
- `interrupts.c` split into `trap.c` (IDT/PIC/IRQ) + `syscall.c`
- App ELFs loaded from StinkFS by name (no fixed-LBA TOC)

## [0.3.0] -- pre-multitasking

First tagged release. Single ring-3 app at a time, full graphical menu,
StinkFS, e1000 + TCP/UDP stack, SB16 audio with software mixer, Doom1 /
Doom2 / FreeDM bundled, ATA installer, `stink-pkg` package manager
scaffold. See the `v0.3.0` git tag for the exact commit.
