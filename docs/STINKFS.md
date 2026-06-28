# StinkFS On-Disk Format

A deliberately tiny named-file filesystem. No directories, no permissions,
no inodes -- just a fixed-position directory listing each file's start LBA
and byte length. The format is the source of truth for app ELFs, WAD
assets, savegames, the package database, and any persistent userland state.

## Disk layout

```
LBA 0           Boot sector (one 512-byte BIOS-loaded block, jumps to PM)
LBA 0           Bootblock sector 0 (boot.s real-mode prologue + GDT)
LBA 1   .. 15   Bootblock tail (boot.s pm_entry + compiled bootmain.c)
LBA 16  .. 511  Kernel ELF (stripped; 496 sectors = 248 KiB headroom)
LBA 512 .. 513  StinkFS directory (2 sectors, see "Directory" below)
LBA 514 .. END  StinkFS data region (~100 MiB by default)
```

`END = FS_DATA_END` in `kernel/fs/fs.c` (`200514` today); the Makefile
keeps this in lock-step via `DISK_END = FS_DATA_END * 512`. Resizing the
filesystem is one constant in each place.

The bootblock + kernel split (LBA 0..15 + 16..511) is the result of
TODO §13 (ELF-aware bootloader): `boot/bootmain.c` walks the kernel
ELF's PT_LOAD program headers, so kernel growth no longer requires
re-laying-out the disk image. Bumping past 248 KiB just means
raising `FS_DIR_LBA` in `Makefile` and `kernel/fs/fs.c`.

Everything is little-endian and packed. There is no FAT, no superblock,
no metadata outside the directory.

## Directory (2 sectors @ LBA 512)

```c
#define STINKFS_MAGIC 0x4B4E5453   /* "STNK" little-endian */
#define FS_MAX_FILES  40

struct fs_file {
    char         name[16];   /* NUL-padded canonical name (15 visible + NUL) */
    unsigned int start;      /* absolute LBA of the file's first data sector */
    unsigned int size;       /* length in bytes (0..size <= sectors*512)     */
};                           /* sizeof = 24 bytes, packed                    */

struct fs_dir {
    unsigned int   magic;        /* STINKFS_MAGIC; rewritten on first use     */
    unsigned int   count;        /* number of populated `files[]` entries     */
    unsigned int   next_free;    /* next unallocated data sector (LBA)        */
    struct fs_file files[FS_MAX_FILES];
};                               /* 12 + 40 * 24 = 972 bytes, fits in 2 sectors */
```

Magic mismatch on boot means "uninitialised disk" -- the kernel rewrites a
fresh empty directory at `LBA 512` (count=0, next_free=`FS_DATA_LBA`).
That is the only place an uninitialised disk gets written without an
explicit user action.

## Allocation

Every file is **contiguous on disk** -- always. There is no extent list and
no fragmentation handling. When a write needs more sectors than the file
already occupies the kernel allocates a fresh run starting at `next_free`,
bumps `next_free`, and abandons the old run if the file is being replaced.
This is simple to reason about but means *delete + create + delete + ...*
loops eventually exhaust the data region; a future compaction pass would
sweep gaps closed.

## Operations

All ops resolve files by their 16-byte canonical name -- case-insensitive
on lookup, case-preserving on store, padded with NUL. Trailing NULs are
ignored when comparing.

| Operation             | Returns               | Notes                                   |
|-----------------------|-----------------------|-----------------------------------------|
| `fs_file_write`       | 0 / -1                | replaces; reallocates if size grew      |
| `fs_file_write_at`    | 0 / -1                | random write; pads with zeros           |
| `fs_file_append`      | 0 / -1                | grows in place if next_free, else moves |
| `fs_file_read`        | bytes, -1             | full read                               |
| `fs_file_read_at`     | bytes, -1             | random read from `offset`               |
| `fs_file_delete`      | 0 / -1                | removes the directory entry only        |
| `fs_file_size`        | size, -1              | lookup by name                          |
| `fs_file_count`       | count                 | active entries                          |
| `fs_file_info`        | 0 / -1                | walk directory by index                 |
| `fs_file_lba_sectors` | 0 / -1                | absolute LBA + sector span for loader   |

Every mutating op writes the directory back via `ata_write(LBA 128, 2 sectors)`
at the end of the call. Data sectors are written through a single
`io_buf[512]` bounce buffer.

## VFS layer

`kernel/fs/vfs.c` puts POSIX-ish descriptors (`O_CREATE`, byte cursor,
`SEEK_*`) on top of the named-file API. Each `struct proc` carries a
`fd_table[VFS_FD_MAX]` so per-process FDs cannot collide. Descriptors
remember the file by **name** (not LBA), so a delete + recreate elsewhere
in the directory does not invalidate the cursor -- although the size may
change underneath, and reads past EOF still return zero bytes.

## Image build

The on-disk image is assembled by `tools/make-stinkfs.py`:

1. Boot sector + kernel padded to `IMG_MIN` (64 KiB).
2. A fresh empty directory at `LBA 128`.
3. App ELFs written in a fixed order (so the menu's positional layout is
   reproducible across builds).
4. Optional asset files (Freedoom WADs, etc.) packed after the apps.
5. Final pad to `DISK_END` so the bootloader's INT 13h read never runs off
   the end of the image.

The script is deterministic: the same source produces a bit-identical
image, which keeps `make test-headless` happy and lets the installer
sanity-check that the cloned disk matches the source byte-for-byte.

## Limits and what is intentionally absent

| Limit                       | Today                       | Why                                       |
|-----------------------------|-----------------------------|-------------------------------------------|
| Max files                   | 40                          | Fits the directory in 2 sectors           |
| Max file name length        | 15 visible chars + NUL      | Aligns to a 16-byte canonical id          |
| Data region                 | ~100 MiB                    | Bound by Makefile `DISK_END`              |
| Permissions                 | none                        | Single-user kernel                        |
| Directories / hierarchy     | none                        | Flat namespace is enough today            |
| Journal / crash safety      | none                        | Power loss mid-write can corrupt the dir  |
| Compaction / defragment     | none                        | Delete leaks the abandoned run            |
| Symlinks / hard links       | none                        | Out of scope                              |
| Extended attributes / xattr | none                        | Out of scope                              |

A future revision can add a free-bitmap + extent list for non-contiguous
allocation, but no current consumer wants it; everything in the codebase
treats files as one continuous LBA run.
