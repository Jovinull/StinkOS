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
  `PACKAGING.md` / `MEMORY.md` / `TUTORIAL.md`, plus `COPYING.GPL`
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
