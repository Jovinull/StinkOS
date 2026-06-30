#!/usr/bin/env python3
"""Boot StinkOS in QEMU and capture screenshots used by the README + the
GitHub Pages landing.

Usage:
    bash tools/fetch-wads.sh        # one-time, downloads Freedoom WADs
    make all                        # build os.bin
    python3 tools/capture-screens.py

The script writes PNGs to images/ in the project root. PIL/Pillow is not
required -- a tiny pure-stdlib PPM->PNG writer is inlined below.

CRITICAL: QEMU MUST RUN WITH -snapshot
==========================================================================
Without -snapshot, QEMU writes guest disk modifications back to os.bin
between captures: shell command history (SHELL.HIS), game high scores,
StinkFS dir updates when a file grows. Two captures in a row see different
state -- the second one boots from a half-modified disk, the menu can
come up empty, screenshots get corrupted.

With -snapshot, every write goes to a temporary copy that QEMU discards
on exit. os.bin stays exactly as `make all` produced it. Runs are
idempotent.

Same rule applies to any other automated QEMU run that you want
reproducible: -snapshot. Production `make run` and `make run-install`
do NOT use it (you want SHELL.HIS to persist there), but `make
test-headless` does, and so does this capture script. See
docs/TESTING.md for the full rationale.
"""
import os, socket, struct, subprocess, sys, time, zlib

OS_BIN = "os.bin"
OUT    = "images"
os.makedirs(OUT, exist_ok=True)

MON = "/tmp/sos-capture.mon"
try: os.remove(MON)
except FileNotFoundError: pass

# ---- pure-stdlib PPM -> PNG ------------------------------------------
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
        raw.append(0)                                  # PNG filter byte: None
        raw += rgb[y * stride:(y + 1) * stride]
    with open(png_path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(_chunk(b"IHDR",
                       struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(_chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(_chunk(b"IEND", b""))

# ---- qemu orchestration ----------------------------------------------
if not os.path.exists(OS_BIN):
    sys.exit(f"{OS_BIN} missing -- run `make all` first")

print(">>> launching qemu (headless, -snapshot)")
qemu = subprocess.Popen([
    "qemu-system-i386",
    "-drive", f"format=raw,file={OS_BIN}",
    "-snapshot",                                       # NEVER drop this flag
    "-display", "none",
    "-monitor", f"unix:{MON},server,nowait",
    "-audiodev", "none,id=snd0",
    "-device", "sb16,audiodev=snd0",
    "-netdev", "user,id=net0",
    "-device", "e1000,netdev=net0",
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

def shot(name, pause_before=0.0):
    if pause_before: time.sleep(pause_before)
    ppm = f"/tmp/sos-capture-{name}.ppm"
    if os.path.exists(ppm): os.remove(ppm)
    send(f"screendump {ppm}", pause=0.8)
    if not os.path.exists(ppm) or os.path.getsize(ppm) < 1000:
        print(f"    !! {name} screendump missing or empty"); return
    out = f"{OUT}/{name}.png"
    _ppm_to_png(ppm, out)
    os.remove(ppm)
    print(f"    [+] {name} -> {out} ({os.path.getsize(out)//1024} KiB)")

def key(k, pause=0.25): send(f"sendkey {k}", pause=pause)
def keys(s, p=0.08):
    # QEMU monitor sendkey aliases: literal space + hyphen aren't valid
    # keynames. Translate so the shell sees the right characters; without
    # this `keys("run rs-life")` lost the space + dash and the shell got
    # "runrslife" → ended up in the stink-pkg "package not installed"
    # branch instead of dispatching `run rs-life`.
    aliases = {" ": "spc", "-": "minus", ".": "dot", "_": "underscore"}
    for c in s:
        key(aliases.get(c, c.lower()), pause=p)

# --- 1. menu --------------------------------------------------------
print(">>> waiting for menu (boot + DHCP)")
time.sleep(12)
shot("menu")

# --- 2. FBDEMO (app idx 24) ----------------------------------------
print(">>> FBDEMO")
for _ in range(24): key("s", pause=0.04)
key("ret", pause=0.0); time.sleep(2.5)
shot("fbdemo", pause_before=1.5)
key("x"); time.sleep(1.5)

# --- 3. SHELL + chained commands (cursor at idx 24 -> -10 = SHELL 14) -
print(">>> SHELL + chained help/mem/ps/netinfo")
for _ in range(10): key("w", pause=0.04)
key("ret", pause=0.0); time.sleep(2.0)
keys("help"); key("ret", pause=0.4)
keys("mem");  key("ret", pause=0.4)
keys("ps");   key("ret", pause=0.4)
keys("netinfo"); key("ret", pause=0.4)
shot("shell_status", pause_before=0.5)

# --- 3b. SHELL bg demo: fork + exec ANIM in the background, parent
# shell stays at the prompt and runs `ps` to prove it's still alive.
# Captures the moment after `ps` so the listing shows TWO procs:
# the shell (parent) and ANIM (forked child). This is the visible
# proof of §1 multitasking proper (per-process pgdir + sys_fork +
# sys_exec). Same scenario the smoke-multiproc target exercises.
print(">>> SHELL bg anim (multi-proc demo)")
keys("bg anim"); key("ret", pause=0.6)
time.sleep(2.5)                                   # ANIM reaches ring3, prints lines
keys("ps");      key("ret", pause=0.6)
shot("shell_bg_demo", pause_before=0.5)
keys("exit"); key("ret"); time.sleep(1.5)

# --- 4. Doom (idx 21 -> +7 from SHELL 14) ---------------------------
print(">>> DOOM1 (Freedoom1 -- needs wads/freedoom1.wad)")
for _ in range(7): key("s", pause=0.04)
key("ret", pause=0.0); time.sleep(22.0)                # WAD load + zone init
key("spc"); time.sleep(5.0)
key("spc"); time.sleep(1.5)
key("ret"); time.sleep(1.0)
key("ret"); time.sleep(1.0)
key("up"); time.sleep(0.4)
key("up"); time.sleep(0.4)
key("ret"); time.sleep(6.0)
time.sleep(2.0)
shot("doom_e1m1", pause_before=0.5)
keys("ww", p=0.4)
key("rgt", pause=0.3)
shot("doom_walking", pause_before=0.5)

# --- 5. rs-life (Rust Conway's Game of Life) ------------------------
# After Doom quits we're back at the menu. Cursor is wherever Doom left
# it; rs-life sits past it in the menu so walk down by a generous
# margin then locate it. Simpler: type into the SHELL once it lands.
# We already exited shell above, so navigate back to SHELL (slot 14)
# and `run rs-life`. After a few seconds of the glider gun running,
# capture the framebuffer with multiple gliders visibly streaming
# diagonally.
print(">>> rs-life (Rust Conway's Game of Life)")
# Back to menu, find SHELL again (cursor on DOOM1 -> walk to SHELL).
for _ in range(7): key("w", pause=0.04)
key("ret", pause=0.0); time.sleep(2.0)
keys("run rs-life"); key("ret", pause=0.6)
time.sleep(8.0)                                   # let glider gun emit a few gliders
shot("rs_life", pause_before=0.5)
key("q"); time.sleep(1.5)

print(">>> quitting qemu")
send("quit"); time.sleep(0.5)
try: qemu.wait(timeout=5)
except subprocess.TimeoutExpired: qemu.kill()
print(">>> done -- check images/")
