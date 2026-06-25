#!/usr/bin/env python3
# Build the StinkOS app table-of-contents (a tiny on-disk "filesystem").
# Layout: [u32 count][ entries... ], entry = name[16] + lba(u32 LE) + sectors(u32 LE).
# 'sectors' is derived from each app binary's size.
# Usage: make-toc.py <out> "<name>:<lba>:<binpath>" ...
import sys, os, struct, math

out = sys.argv[1]
specs = sys.argv[2:]

entries = []
for spec in specs:
    name, lba, path = spec.split(":")
    sectors = max(1, math.ceil(os.path.getsize(path) / 512))
    entries.append((name, int(lba), sectors))

# Guard: an app must fit in its slot, i.e. not run into the next app's LBA.
# Catches an app outgrowing its allotted sectors before it silently corrupts
# the neighbouring slot on disk (the same class of bug as a too-small KSECTORS).
for a, b in zip(sorted(entries, key=lambda e: e[1]), sorted(entries, key=lambda e: e[1])[1:]):
    if a[1] + a[2] > b[1]:
        sys.exit("make-toc: app '%s' (LBA %d, %d sectors) overruns '%s' at LBA %d"
                 % (a[0], a[1], a[2], b[0], b[1]))

data = struct.pack("<I", len(entries))
for name, lba, sectors in entries:
    name_field = name.encode()[:16].ljust(16, b"\x00")
    data += name_field + struct.pack("<II", lba, sectors)

with open(out, "wb") as f:
    f.write(data)
