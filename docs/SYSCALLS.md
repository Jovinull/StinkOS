# StinkOS Syscall ABI

Generated from `kernel/sys/syscall.c`; the C source is the source of truth.

## Convention

* Vector: `int 0x80` (DPL=3).
* Arguments: `eax` = syscall number, `ebx` / `ecx` / `edx` / `esi` = up to
  four positional arguments. Pointer arguments are validated by the kernel
  via `paging_user_range_ok` before any dereference.
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

## Argument-packing helpers

A few syscalls fold two 16-bit values into one 32-bit register to fit the
4-arg ABI ceiling:

* `SYS_FILLRECT`: `ebx = (x << 16) | y`, `ecx = (w << 16) | h`.
* `SYS_BLIT`: `ecx = (x << 16) | y`, `edx = (w << 16) | h`.
* `SYS_GETKEYEVENT`: returns `(flags << 24) | char`. Flag bits live in
  `libstink.h` as `STINK_KEY_*`.

## Pointer-safety rules

Every syscall that takes a userland pointer:

1. Refuses pointers outside the user region.
2. Refuses pointers whose `[addr, addr+len)` range crosses the user-region
   boundary.
3. Returns `-1` (or `0` for SYS_PIPE_READ, matching its EOF sentinel)
   without partial work.

This means a malicious or buggy app can never coax the kernel into reading
or writing kernel pages.

## File / VFS notes

* All `SYS_F*` (8-15) operate on **names** -- the canonical 16-byte
  NUL-padded StinkFS identifier. `SYS_OPEN`/`SYS_READ`/etc. (16-20) work
  through a **per-process descriptor table** (`vfs.c` + `struct proc`).
* `O_CREATE` (flag 1) on `SYS_OPEN` creates the file if it does not yet
  exist; otherwise the open fails on a missing name. No other flags are
  defined.

## Networking notes

* `SYS_SOCK_*` (31-35) operate on TCP handles 0..7. The TCB lifetime ends
  when both peers have finished the FIN handshake; before that the app
  must keep polling `SYS_NET_POLL` to drive the RX path.
* DHCP runs automatically at boot. Until the DORA completes, `SYS_NETINFO`
  returns the link-local zero state and `SYS_PING` fails. `SYS_NETINFO`'s
  `dhcp_state` matches `enum dhcp_state` (0 init, 1 discover, 2 request,
  3 bound, 4 failed).
