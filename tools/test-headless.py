#!/usr/bin/env python3
# Headless boot test for StinkOS.
# Launches the built image in qemu, captures the serial debug log, injects a few
# keystrokes through the qemu monitor, and dumps the screen. Asserts the kernel
# reached protected mode, set the VBE mode, drew to the framebuffer, and that the
# timer and keyboard IRQs fired. Exit 0 on success, 1 on failure.
import subprocess, socket, time, os, signal, sys

SER = "/tmp/stinkos_ser.log"
MON = "/tmp/stinkos_mon.sock"
FB = "/tmp/stinkos_fb.ppm"

for f in (SER, MON, FB):
    try:
        os.remove(f)
    except OSError:
        pass

qemu = subprocess.Popen(
    ["qemu-system-i386", "-drive", "format=raw,file=os.bin", "-display", "none",
     "-serial", "file:" + SER, "-monitor", "unix:%s,server,nowait" % MON,
     "-no-reboot"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def serial():
    try:
        return open(SER).read()
    except OSError:
        return ""


def fail(msg):
    qemu.send_signal(signal.SIGKILL)
    print("FAIL:", msg)
    print("--- serial ---")
    print(serial().strip())
    sys.exit(1)


def read_ppm(path):
    try:
        data = open(path, "rb").read()
    except OSError:
        return (0, 0, b"")
    if not data.startswith(b"P6"):
        return (0, 0, b"")
    toks, j = [], 0                       # tokenize: magic, width, height, maxval
    while len(toks) < 4 and j < len(data):
        while j < len(data) and data[j:j + 1].isspace():
            j += 1
        start = j
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        toks.append(data[start:j])
    j += 1                                # single whitespace after maxval
    return (int(toks[1]), int(toks[2]), data[j:])


def nonblack_count(px):
    return sum(1 for k in range(0, len(px) - 2, 3)
               if px[k] or px[k + 1] or px[k + 2])


def pixel_at(width, px, x, y):
    o = (y * width + x) * 3
    return (px[o], px[o + 1], px[o + 2]) if o + 2 < len(px) else (0, 0, 0)


# Wait for the start menu to be drawn and ready for input.
for _ in range(60):
    if "menu: ready" in serial():
        break
    time.sleep(0.1)
else:
    fail("kernel did not reach 'menu: ready'")

# Drive the monitor: inject keystrokes, then dump the screen.
sock = socket.socket(socket.AF_UNIX)
for _ in range(50):
    try:
        sock.connect(MON)
        break
    except OSError:
        time.sleep(0.1)
else:
    fail("could not connect to qemu monitor")

time.sleep(0.2)
try:
    sock.recv(4096)        # consume the monitor banner
except OSError:
    pass
# a,b,c: exercise the keyboard IRQ.  s,w: move the menu cursor and back.
# ret: launch the highlighted app (HELLO), which draws and waits for a key.
for key in ("a", "b", "c", "s", "w", "ret"):
    sock.sendall(("sendkey %s\n" % key).encode())
    time.sleep(0.2)
time.sleep(0.4)                                 # let HELLO draw and reach its key poll
sock.sendall(("screendump %s\n" % FB).encode())  # capture while HELLO is on screen
time.sleep(0.4)
sock.sendall(b"sendkey x\n")                      # HELLO gets key -> logs -> SYS_EXIT
time.sleep(0.5)                                   # ... and the menu redraws

out = serial()
w, h, px = read_ppm(FB)
drawn = nonblack_count(px)
pr, pg, pb = pixel_at(w, px, 15, 15)      # inside the app's 20x20 white square
app_drew = pr > 200 and pg > 200 and pb > 200
tr, tg, tb = pixel_at(w, px, 122, 90)     # a lit pixel of the "STINKOS" title
title_drawn = tr > 200 and tg > 200 and tb > 200
mr, mg, mb = pixel_at(w, px, 142, 120)    # a lit pixel of the "1 HELLO" menu entry
menu_drawn = mr > 200 and mg > 200 and mb > 200
qemu.send_signal(signal.SIGKILL)

checks = {
    "protected mode":  "StinkOS: protected mode active" in out,
    "gdt+tss":         "gdt: kernel+user segments and tss loaded" in out,
    "paging":          "paging: enabled" in out,
    "pmm alloc":       "pmm: frame 0x" in out,
    "VBE mode":        "vbe: 1024x768" in out,
    "framebuffer draw": drawn > 100000,
    "text render":     title_drawn,
    "menu ready":      "menu: ready" in out,
    "menu drawn":      menu_drawn,
    "timer IRQ":       "StinkOS: timer tick" in out,
    "keyboard IRQ":    all(("kbd: " + c) in out for c in "abc"),
    "disk app loaded": "loader: app loaded from disk slot" in out,
    "ring3 syscall":   "ring3: hello from disk app" in out,
    "draw syscall":    app_drew,
    "alloc syscall":   "app: alloc ok" in out,
    "getkey syscall":  "app: key received" in out,
    "exit to menu":    "menu: back" in out,
}
missing = [name for name, ok in checks.items() if not ok]
if missing:
    print("FAIL:", ", ".join(missing), "not verified  (framebuffer pixels=%d)" % drawn)
    print("--- serial ---")
    print(out.strip())
    sys.exit(1)

print("PASS: full cycle - menu -> launch disk app in ring3 (log/draw/getkey/alloc) -> exit -> menu")
