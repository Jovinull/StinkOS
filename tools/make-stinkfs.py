#!/usr/bin/env python3
# Bundle a set of files into a fresh StinkFS region in an existing disk image.
# Replaces the StinkFS directory sectors and the data area starting at the data
# LBA; everything else in the image (boot sector, kernel) is left untouched.
#
# Layout written (must match the kernel's fs.c):
#   2 dir sectors -> [u32 magic][u32 count][u32 next_free][FS_MAX_FILES * fs_file_entry]
#   fs_file        -> [16-byte name][u32 start_lba][u32 size_bytes]
#
# Usage:
#   make-stinkfs.py <image> <dir_lba> <data_lba> <data_end_lba> \
#                   NAME1=path1 NAME2=path2 ...
#
# Each NAME is truncated to 16 bytes (NUL-padded). The data is written
# contiguously starting at <data_lba>; rejects with an error if the total
# would overflow the region.
import os
import struct
import sys

STINKFS_MAGIC = 0x4B4E5453   # 'S','T','N','K' little-endian
SECTOR = 512
DIR_SECTORS = 4              # directory spans four 512-byte sectors (2 KiB)
NAME_LEN = 16
MAX_FILES = 80

def main():
    if len(sys.argv) < 5:
        sys.exit("usage: make-stinkfs.py <image> <dir_lba> <data_lba> "
                 "<data_end_lba> NAME=path ...")

    image = sys.argv[1]
    dir_lba = int(sys.argv[2])
    data_lba = int(sys.argv[3])
    data_end_lba = int(sys.argv[4])
    specs = sys.argv[5:]

    if len(specs) > MAX_FILES:
        sys.exit(f"stinkfs: too many files ({len(specs)}); MAX_FILES={MAX_FILES}")

    entries = []
    next_free = data_lba

    for spec in specs:
        if '=' not in spec:
            sys.exit(f"stinkfs: bad spec '{spec}', expected NAME=path")
        name, path = spec.split('=', 1)
        if not os.path.isfile(path):
            sys.exit(f"stinkfs: '{path}' not found")
        size = os.path.getsize(path)
        sectors = (size + SECTOR - 1) // SECTOR
        if next_free + sectors > data_end_lba:
            sys.exit(f"stinkfs: '{name}' ({size} B) would overflow data region "
                     f"(next_free={next_free}, end={data_end_lba}, needs {sectors})")
        entries.append((name, path, next_free, size))
        next_free += sectors

    with open(image, 'r+b') as f:
        # Write file contents into the data region.
        for name, path, start_lba, size in entries:
            with open(path, 'rb') as src:
                data = src.read()
            f.seek(start_lba * SECTOR)
            # Pad to sector boundary so a short read at EOF still lands cleanly.
            padded_len = ((size + SECTOR - 1) // SECTOR) * SECTOR
            f.write(data + b'\x00' * (padded_len - size))

        # Build the directory (two sectors = 1024 bytes).
        dir_bytes = struct.pack('<III', STINKFS_MAGIC, len(entries), next_free)
        for name, _path, start_lba, size in entries:
            name_bytes = name.encode('ascii')[:NAME_LEN].ljust(NAME_LEN, b'\x00')
            dir_bytes += name_bytes + struct.pack('<II', start_lba, size)
        # Pad unused file slots so the kernel reads zeros.
        empty_entry = b'\x00' * (NAME_LEN + 8)
        dir_bytes += empty_entry * (MAX_FILES - len(entries))
        # Pad to exactly two sectors.
        dir_total = SECTOR * DIR_SECTORS
        if len(dir_bytes) > dir_total:
            sys.exit(f"stinkfs: directory ({len(dir_bytes)} B) exceeds {dir_total} B "
                     f"({DIR_SECTORS} sectors)")
        dir_bytes += b'\x00' * (dir_total - len(dir_bytes))

        f.seek(dir_lba * SECTOR)
        f.write(dir_bytes)

    total_bytes = sum(size for _, _, _, size in entries)
    print(f"stinkfs: wrote {len(entries)} files ({total_bytes} B), "
          f"next_free=LBA {next_free}")


if __name__ == '__main__':
    main()
