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


def nonblack_pixels(path):
    try:
        data = open(path, "rb").read()
    except OSError:
        return 0
    if not data.startswith(b"P6"):
        return 0
    # skip the 3 whitespace-separated header fields (magic, WxH, maxval)
    i, fields = 0, 0
    while fields < 3 and i < len(data):
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        while i < len(data) and not data[i:i + 1].isspace():
            i += 1
        fields += 1
    i += 1  # single whitespace after maxval
    px = data[i:]
    return sum(1 for j in range(0, len(px) - 2, 3)
               if px[j] or px[j + 1] or px[j + 2])


# Wait for the kernel to finish bringing up interrupts.
for _ in range(60):
    if "interrupts enabled" in serial():
        break
    time.sleep(0.1)
else:
    fail("kernel did not reach 'interrupts enabled'")

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
for key in ("a", "b", "c"):
    sock.sendall(("sendkey %s\n" % key).encode())
    time.sleep(0.2)
sock.sendall(("screendump %s\n" % FB).encode())
time.sleep(0.6)

out = serial()
drawn = nonblack_pixels(FB)
qemu.send_signal(signal.SIGKILL)

checks = {
    "protected mode":  "StinkOS: protected mode active" in out,
    "paging":          "paging: enabled" in out,
    "pmm alloc":       "pmm: frame 0x" in out,
    "VBE mode":        "vbe: 1024x768" in out,
    "framebuffer draw": drawn > 100000,
    "timer IRQ":       "StinkOS: timer tick" in out,
    "keyboard IRQ":    all(("kbd: " + c) in out for c in "abc"),
}
missing = [name for name, ok in checks.items() if not ok]
if missing:
    print("FAIL:", ", ".join(missing), "not verified  (framebuffer pixels=%d)" % drawn)
    print("--- serial ---")
    print(out.strip())
    sys.exit(1)

print("PASS: protected mode + paging + pmm + VBE + framebuffer (%d px) + timer + keyboard" % drawn)
