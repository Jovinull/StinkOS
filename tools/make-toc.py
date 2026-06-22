#!/usr/bin/env python3
# Build the StinkOS app table-of-contents (a tiny on-disk "filesystem").
# Layout: [u32 count][ entries... ], entry = name[16] + lba(u32 LE) + sectors(u32 LE).
# 'sectors' is derived from each app binary's size.
# Usage: make-toc.py <out> "<name>:<lba>:<binpath>" ...
import sys, os, struct, math

out = sys.argv[1]
specs = sys.argv[2:]

data = struct.pack("<I", len(specs))
for spec in specs:
    name, lba, path = spec.split(":")
    sectors = max(1, math.ceil(os.path.getsize(path) / 512))
    name_field = name.encode()[:16].ljust(16, b"\x00")
    data += name_field + struct.pack("<II", int(lba), sectors)

with open(out, "wb") as f:
    f.write(data)
