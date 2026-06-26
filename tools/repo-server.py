#!/usr/bin/env python3
"""Reference HTTP repository for stink-pkg.

Serves two routes from a directory of .stinkpkg archives:

  GET /index.txt          regenerated on every request:
                          "<name> <version> <sha256>\n" per package
  GET /pkg/<name>.stinkpkg the raw archive bytes

Pass --pkgdir to point at the directory holding the packages
(default: ./repo). The index reads each package's header to extract
the embedded name/version, then hashes the full file -- so renaming
or replacing a file on disk is enough; no manifest to keep in sync.

Usage:
    repo-server.py --pkgdir ./repo --bind 0.0.0.0 --port 8080
"""

import argparse
import hashlib
import os
import struct
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

MAGIC          = 0x474B5053
HEADER_FMT     = "<IHH32s16s128sIIII"
HEADER_SIZE    = struct.calcsize(HEADER_FMT)            # 196

PKGDIR = None                                            # set in main()


def cstr(b):
    """Trim a NUL-padded fixed-length C string to its visible bytes."""
    end = b.find(b"\x00")
    return (b if end < 0 else b[:end]).decode("ascii", errors="replace")


def parse_pkg(path):
    """Return (name, version) read from a .stinkpkg header, or None on error."""
    with open(path, "rb") as f:
        head = f.read(HEADER_SIZE)
    if len(head) < HEADER_SIZE:
        return None
    fields = struct.unpack(HEADER_FMT, head)
    magic, _ver, _flags, name_b, ver_b, _desc, _dc, _fc, _po, _ps = fields
    if magic != MAGIC:
        return None
    return cstr(name_b), cstr(ver_b)


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(64 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def build_index(pkgdir):
    """Emit one line per *.stinkpkg in `pkgdir`. Skips malformed files."""
    lines = []
    for fn in sorted(os.listdir(pkgdir)):
        if not fn.endswith(".stinkpkg"):
            continue
        path = os.path.join(pkgdir, fn)
        meta = parse_pkg(path)
        if meta is None:
            continue
        name, version = meta
        sha = sha256_of(path)
        lines.append(f"{name} {version} {sha}\n")
    return "".join(lines).encode("ascii")


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/index.txt":
            body = build_index(PKGDIR)
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if self.path.startswith("/pkg/") and self.path.endswith(".stinkpkg"):
            name = os.path.basename(self.path)
            path = os.path.join(PKGDIR, name)
            if not os.path.isfile(path):
                self.send_error(404, "not found")
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(os.path.getsize(path)))
            self.end_headers()
            with open(path, "rb") as f:
                while True:
                    chunk = f.read(64 * 1024)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
            return

        self.send_error(404, "not found")

    def log_message(self, fmt, *args):
        sys.stderr.write(f"{self.address_string()} - {fmt % args}\n")


def main():
    global PKGDIR
    ap = argparse.ArgumentParser(
        description="Reference HTTP server for stink-pkg.")
    ap.add_argument("--pkgdir", default="./repo",
                    help="directory of .stinkpkg files")
    ap.add_argument("--bind",   default="0.0.0.0", help="bind address")
    ap.add_argument("--port",   type=int, default=8080, help="bind port")
    args = ap.parse_args()

    PKGDIR = os.path.abspath(args.pkgdir)
    if not os.path.isdir(PKGDIR):
        sys.exit(f"pkgdir does not exist: {PKGDIR}")

    httpd = HTTPServer((args.bind, args.port), Handler)
    print(f"serving {PKGDIR} on http://{args.bind}:{args.port}")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nshutting down")


if __name__ == "__main__":
    sys.exit(main())
