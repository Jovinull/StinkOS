#!/usr/bin/env python3
"""One-shot capture: boot StinkOS, navigate shell, run rs-life,
screendump after the glider gun has emitted a few gliders. Standalone
because the main capture-screens.py is already mid-Doom by the time
rs-life would slot in -- cleaner to spin a fresh QEMU just for this
one frame."""
import os, socket, subprocess, sys, time, struct, zlib

OS_BIN = "os.bin"
MON    = "/tmp/sos-rslife-mon.sock"
OUT    = "images"

if not os.path.exists(OS_BIN):
    sys.exit(f"{OS_BIN} missing -- run `make all` first")

# Pure-stdlib PPM -> PNG (copy from capture-screens.py).
def _chunk(tag, data):
    body = tag + data
    return struct.pack(">I", len(data)) + body + \
           struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

def _ppm_to_png(ppm_path, png_path):
    with open(ppm_path, "rb") as f:
        if f.readline().strip() != b"P6":
            raise RuntimeError(f"not a P6 PPM: {ppm_path}")
        line = f.readline().strip()
        while line.startswith(b"#"):
            line = f.readline().strip()
        w, h = map(int, line.split())
        if int(f.readline().strip()) != 255:
            raise RuntimeError("unsupported PPM maxval")
        rgb = f.read(w * h * 3)
    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)
        raw += rgb[y * stride:(y + 1) * stride]
    with open(png_path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(_chunk(b"IHDR",
                       struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(_chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(_chunk(b"IEND", b""))

if os.path.exists(MON): os.remove(MON)

print(">>> launching qemu (headless, -snapshot)")
qemu = subprocess.Popen([
    "qemu-system-i386",
    "-drive", f"format=raw,file={OS_BIN}",
    "-snapshot",
    "-display", "none",
    "-monitor", f"unix:{MON},server,nowait",
    "-cpu", "Penryn",
], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

for _ in range(40):
    if os.path.exists(MON): break
    time.sleep(0.1)

sock = socket.socket(socket.AF_UNIX)
sock.connect(MON)
sock.settimeout(2.0)
time.sleep(0.3)
try: sock.recv(8192)
except OSError: pass

def send(cmd, pause=0.25):
    sock.sendall((cmd + "\n").encode())
    time.sleep(pause)
    try:
        while sock.recv(8192): pass
    except OSError: pass

def key(k, pause=0.25): send(f"sendkey {k}", pause=pause)

def keys(s, p=0.10):
    aliases = {" ": "spc", "-": "minus", ".": "dot", "_": "underscore"}
    for c in s:
        key(aliases.get(c, c.lower()), pause=p)

print(">>> waiting for menu (boot + DHCP)")
time.sleep(12)

# Walk to SHELL (idx 14) and enter.
for _ in range(14): key("s", pause=0.04)
key("ret", pause=0.0); time.sleep(2.0)

# Launch rs-life via shell.
keys("run rs-life"); key("ret", pause=0.6)

# Let glider gun emit gliders. ~10 gens/sec, 12s gives ~120 gens.
time.sleep(12.0)

ppm = "/tmp/sos-rslife.ppm"
if os.path.exists(ppm): os.remove(ppm)
send(f"screendump {ppm}", pause=1.2)
if not os.path.exists(ppm) or os.path.getsize(ppm) < 1000:
    print("    !! rs_life screendump missing or empty")
else:
    out = f"{OUT}/rs_life.png"
    _ppm_to_png(ppm, out)
    os.remove(ppm)
    print(f"    [+] rs_life -> {out} ({os.path.getsize(out)//1024} KiB)")

# Quit rs-life cleanly then quit qemu.
key("q", pause=1.0)
send("quit"); time.sleep(0.5)
try: qemu.wait(timeout=5)
except subprocess.TimeoutExpired: qemu.kill()
print(">>> done")
