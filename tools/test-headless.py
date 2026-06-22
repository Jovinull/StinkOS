#!/usr/bin/env python3
# Headless boot test for StinkOS.
# Launches the built image in qemu, captures the serial debug log, injects a few
# keystrokes through the qemu monitor, then asserts the kernel reached protected
# mode, the PIT timer fired, and the keyboard driver decoded the input.
# Exit 0 on success, 1 on failure. Requires os.bin to be built already.
import subprocess, socket, time, os, signal, sys

SER = "/tmp/stinkos_ser.log"
MON = "/tmp/stinkos_mon.sock"

for f in (SER, MON):
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


# Wait for the kernel to finish bringing up interrupts.
for _ in range(60):
    if "interrupts enabled" in serial():
        break
    time.sleep(0.1)
else:
    fail("kernel did not reach 'interrupts enabled'")

# Inject keystrokes via the monitor socket.
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
time.sleep(0.4)

out = serial()
qemu.send_signal(signal.SIGKILL)

checks = {
    "protected mode": "StinkOS: protected mode active" in out,
    "VBE mode":       "vbe: 1024x768" in out,
    "timer IRQ":      "StinkOS: timer tick" in out,
    "keyboard IRQ":   all(("kbd: " + c) in out for c in "abc"),
}
missing = [name for name, ok in checks.items() if not ok]
if missing:
    print("FAIL:", ", ".join(missing), "not verified")
    print("--- serial ---")
    print(out.strip())
    sys.exit(1)

print("PASS: protected mode + VBE mode + timer IRQ + keyboard IRQ verified")
