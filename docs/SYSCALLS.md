# StinkOS Syscall ABI

Generated from `kernel/sys/syscall.c`; the C source is the source of truth.

## Convention

* Vector: `int 0x80` (DPL=3).
* Arguments: `eax` = syscall number, `ebx` / `ecx` / `edx` / `esi` = up to
  four positional arguments. Pointer arguments are validated against the
  caller's page tables before any dereference (see Pointer-safety rules).
* Return: result in `eax`. A `-1` (i.e. `0xFFFFFFFF`) indicates failure
  unless a syscall explicitly documents another sentinel.
* libstink wrappers in `lib/libstink.h` provide a `sys_*` prefix for each
  entry below.

## Number table

| #  | Name                | Args (ebx, ecx, edx, esi)                | Returns                                   |
|----|---------------------|------------------------------------------|-------------------------------------------|
|  1 | `SYS_LOG`           | `const char *msg`                         | 0                                         |
|  2 | `SYS_DRAW`          | `int x, int y, u32 rgb`                   | 0                                         |
|  3 | `SYS_GETKEY`        | -                                         | byte or 0 if no key                       |
|  4 | `SYS_ALLOC`         | -                                         | user vaddr of next page, 0 if OOM         |
|  5 | `SYS_EXIT`          | -                                         | does not return                           |
|  6 | `SYS_TICKS`         | -                                         | PIT ticks since boot                      |
|  7 | `SYS_SOUND`         | `u32 freq` (0 silences)                   | 0                                         |
|  8 | `SYS_FWRITE`        | `name, buf, size`                         | 0 / -1                                    |
|  9 | `SYS_FREAD`         | `name, buf, max`                          | byte count, -1                            |
| 10 | `SYS_FCOUNT`        | -                                         | StinkFS file count                        |
| 11 | `SYS_FINFO`         | `int index, char name[16]`                | 0 / -1                                    |
| 12 | `SYS_FDELETE`       | `name`                                    | 0 / -1                                    |
| 13 | `SYS_FAPPEND`       | `name, buf, size`                         | 0 / -1                                    |
| 14 | `SYS_FREAD_AT`      | `name, buf, max, offset`                  | byte count, -1                            |
| 15 | `SYS_FWRITE_AT`     | `name, buf, size, offset`                 | 0 / -1                                    |
| 16 | `SYS_OPEN`          | `name, flags`                             | fd or -1                                  |
| 17 | `SYS_CLOSE`         | `fd`                                      | 0 / -1                                    |
| 18 | `SYS_READ`          | `fd, buf, n`                              | byte count, -1                            |
| 19 | `SYS_WRITE`         | `fd, buf, n`                              | byte count, -1                            |
| 20 | `SYS_SEEK`          | `fd, offset, whence`                      | new cursor, -1                            |
| 21 | `SYS_DRAWTEXT`      | `x, y, const char *str, u32 rgb`          | 0 / -1                                    |
| 22 | `SYS_FILLRECT`      | `xy_packed, wh_packed, u32 rgb`           | 0                                         |
| 23 | `SYS_SLEEP_MS`      | `u32 ms`                                  | 0                                         |
| 24 | `SYS_SBRK`          | `int delta` (signed)                      | previous break, -1                        |
| 25 | `SYS_GETKEYEVENT`   | -                                         | packed event (24b char + flags), 0        |
| 26 | `SYS_BLIT`          | `void *src, xy_packed, wh_packed`         | 0                                         |
| 27 | `SYS_GETMOUSE`      | `*dx, *dy, *buttons`                      | 0 / -1                                    |
| 28 | `SYS_AUDIO_PLAY`    | `u8 *samples, len, volume`                | mixer handle 0..7, -1                     |
| 29 | `SYS_AUDIO_STOP`    | `int handle`                              | 0                                         |
| 30 | `SYS_AUDIO_SET_VOLUME` | `int handle, int volume`               | 0                                         |
| 31 | `SYS_SOCK_CONNECT`  | `u32 ipv4, u16 port`                      | TCP handle 0..7, -1                       |
| 32 | `SYS_SOCK_SEND`     | `h, buf, len`                             | bytes queued, -1                          |
| 33 | `SYS_SOCK_RECV`     | `h, buf, max`                             | bytes copied (0 if empty)                 |
| 34 | `SYS_SOCK_CLOSE`    | `h`                                       | 0                                         |
| 35 | `SYS_SOCK_STATE`    | `h`                                       | `enum tcp_state` (CLOSED..TIME_WAIT)      |
| 36 | `SYS_DNS_REQUEST`   | `const char *name`                        | 0 / -1                                    |
| 37 | `SYS_DNS_POLL`      | `u32 *out_ip`                             | 1 ready, 0 pending                        |
| 38 | `SYS_NET_POLL`      | -                                         | 1 frame processed, 0 idle                 |
| 39 | `SYS_DISK_INFO`     | `int drive, char model[41], u32 *sectors` | 0 / -1                                    |
| 40 | `SYS_DISK_COPY`     | `src_drv, dst_drv, count, src_lba`        | sectors copied                            |
| 41 | `SYS_EXEC`          | `const char *name`                        | does not return on success, -1 if no app  |
| 42 | `SYS_MAP_FB`        | -                                         | userland FB base vaddr                    |
| 43 | `SYS_NETINFO`       | `struct net_info *out`                    | 0 / -1                                    |
| 44 | `SYS_PING`          | `u32 ipv4(net order), u32 timeout_ms`     | RTT ms, -1                                |
| 45 | `SYS_GETPID`        | -                                         | caller's PID                              |
| 46 | `SYS_KILL`          | `int pid`                                 | 0 / -1 (PID 1 protected)                  |
| 47 | `SYS_WAIT`          | -                                         | exit code of reaped child, -1             |
| 48 | `SYS_WAITPID`       | `int pid`                                 | exit code, -1                             |
| 49 | `SYS_PIPE`          | `int fds[2]`                              | 0 / -1                                    |
| 50 | `SYS_PIPE_READ`     | `int h, void *buf, u32 n`                 | byte count (0 = EOF), -1                  |
| 51 | `SYS_PIPE_WRITE`    | `int h, const void *buf, u32 n`           | byte count, -1 (EPIPE)                    |
| 52 | `SYS_PIPE_CLOSE`    | `int h`                                   | 0 / -1                                    |
| 53 | `SYS_SIGNAL`        | `int sig, void (*handler)(int)`           | 0 / -1                                    |
| 54 | `SYS_RAISE`         | `int pid, int sig`                        | 0 / -1                                    |
| 55 | `SYS_SIGPOLL`       | -                                         | next pending signal number, 0 if none     |
| 56 | `SYS_MMAP`          | `u32 size`                                | base vaddr, 0 if OOM                      |
| 57 | `SYS_MUNMAP`        | `void *addr, u32 size`                    | 0 / -1                                    |
| 85 | `SYS_WIN_CREATE`    | `u32 w, u32 h, u32 flags`                 | 0 / -1 (buffer mapped at `USER_WIN_BASE`) |
| 86 | `SYS_WIN_SHOW`      | `int x, int y, const char *title`         | 0 / -1 (negative x or y: kernel places it)|
| 87 | `SYS_WIN_FLUSH`     | -                                         | 0                                         |
| 88 | `SYS_WIN_DESTROY`   | -                                         | 0                                         |
| 89 | `SYS_WIN_GET_EVENT` | `struct win_event *out`                   | 0, -1 if the queue is empty               |
| 90 | `SYS_WIN_RAISE`     | -                                         | 0                                         |
| 91 | `SYS_WIN_MOVE`      | `int x, int y`                            | 0                                         |
| 92 | `SYS_CLIP_WRITE`    | `const void *buf, u32 len`                | bytes stored (max 4096), -1               |
| 93 | `SYS_CLIP_READ`     | `void *buf, u32 max`                      | bytes copied                              |
| 94 | `SYS_WIN_RESIZE`    | `u32 w, u32 h`                            | 0 / -1 (old buffer kept on failure)       |
| 95 | `SYS_AUDIO_PLAYING` | `int handle`                              | 1 while the sound plays, else 0           |
| 96 | `SYS_REAP`          | `int *code` (or 0)                        | pid of an exited child, 0 none yet, -1 none |
| 97 | `SYS_WIN_WAIT`      | `u32 timeout_ms`                          | 1 event waiting, 0 timeout, -1 no window  |
| 98 | `SYS_FB_INFO`       | `u32 out[4]` (w, h, pitch bytes, bpp)     | 0 / -1                                    |

The table lists a representative slice; `kernel/sys/syscall.c` has every
case. Numbers 5-40 and 58-84 cover exit, timing, files, text and pixel
drawing, audio, sockets, DNS, disk and process control.

## Blocking and scheduling

The kernel does not preempt itself: a syscall runs to completion with
respect to other processes unless it blocks. `SYS_SLEEP_MS`, `SYS_WAIT`,
`SYS_WAITPID`, `SYS_PING`, `SYS_WIN_WAIT` and pipe reads/writes block the
caller (it uses no CPU) and let other processes run. Everything else
returns without waiting; polling loops should sleep or wait between
polls rather than spin.

`SYS_KILL` is a request: the target exits by itself the next time it
returns to ring 3 (exit code 137). `SYS_WAITPID` on it then succeeds.

## Windows

A process owns at most one window. After `SYS_WIN_CREATE` its pixels
(`w * h` XRGB words, row-major) are mapped read-write at `USER_WIN_BASE`
(0x12000000); the process draws there and calls `SYS_WIN_FLUSH`. The
kernel draws the title bar, close box and border around that client area
and owns the taskbar and pointer.

* Flags: `1` desktop (no chrome, bottom of the stack, not in the
  taskbar), `2` resizable.
* `struct win_event { int type, x, y, buttons, key; }`: `1` mouse (client
  coordinates; `buttons` = state, `key` = buttons that changed; one event
  with `x = y = -1` when the pointer leaves the client area), `2` key
  (`key` = character), `3` close box, `4` resize request (`x`, `y` = new
  client size; answer with `SYS_WIN_RESIZE`), `5` raw key event,
  `6` focus (`x` = 1 gained).
* A window owner's `SYS_GETKEY` / `SYS_GETKEYEVENT` read that window's own
  key queue (filled only while it has focus); `SYS_GETMOUSE` returns no
  motion for it. The full-screen drawing calls (`SYS_DRAW`, `SYS_FILLRECT`,
  `SYS_BLIT`, `SYS_BLIT_SCALED`, `SYS_DRAWTEXT`, `SYS_MAP_FB`) fail with -1
  for a window owner.
* A process without a window that uses those drawing calls while windows
  exist takes the whole screen until it exits.
* The window is destroyed when its owner exits, is killed or execs.

## Argument-packing helpers

A few syscalls fold two 16-bit values into one 32-bit register to fit the
4-arg ABI ceiling:

* `SYS_FILLRECT`: `ebx = (x << 16) | y`, `ecx = (w << 16) | h`.
* `SYS_BLIT`: `ecx = (x << 16) | y`, `edx = (w << 16) | h`.
* `SYS_GETKEYEVENT`: returns `(flags << 24) | char`. Flag bits live in
  `libstink.h` as `STINK_KEY_*`.

## Pointer-safety rules

Every syscall that takes a userland pointer checks the whole range
`[addr, addr+len)` against the caller's page tables first:

1. Every page must be present and user-accessible. Kernel addresses,
   unmapped holes (after `SYS_MUNMAP`), and ranges that wrap are refused.
2. If the kernel is going to write there, every page must be writable. A
   page that is read-only only because it is still shared copy-on-write
   after `fork` is given its private copy first; pages that are really
   read-only (`.text`, `.rodata`) are refused.
3. On refusal the call returns `-1` without partial work.

`CR0.WP` is set, so the kernel cannot write through a read-only mapping
even by mistake. `apps/ktest.c` (`badptr`, `munmap-hole`, `cow-kwrite`,
`text-kwrite`) exercises these rules.

## File / VFS notes

* All `SYS_F*` (8-15) operate on **names** -- the canonical 16-byte
  NUL-padded StinkFS identifier. `SYS_OPEN`/`SYS_READ`/etc. (16-20) work
  through a **per-process descriptor table** (`vfs.c` + `struct proc`).
* `O_CREATE` (flag 1) on `SYS_OPEN` creates the file if it does not yet
  exist; otherwise the open fails on a missing name. No other flags are
  defined.

## Networking notes

* `SYS_SOCK_*` (31-35) operate on TCP handles 0..7, and only on handles
  the calling process opened. Received frames and the protocol timers are
  handled by a kernel thread, so an app does not have to call
  `SYS_NET_POLL` to keep a connection alive (it still may, to process
  pending frames at once).
* DHCP runs automatically at boot. Until the DORA completes, `SYS_NETINFO`
  returns the link-local zero state and `SYS_PING` fails. `SYS_NETINFO`'s
  `dhcp_state` matches `enum dhcp_state` (0 init, 1 discover, 2 request,
  3 bound, 4 failed).
