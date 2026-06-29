#!/usr/bin/env python3
"""Boots the OS, navigates to the SHELL, types `shutdown`, and asserts
both that the ACPI S5 write path fired (kernel log line) and that QEMU
powered off within a sane window. Exit 0 PASS, 1 FAIL.

Note: even when ACPI fires, the actual power-off on QEMU PIIX4 is
served by the legacy port-0x604 fallback because our hardcoded
SLP_TYPa=5 doesn't match QEMU's expected S5 value (which would only
be known via DSDT/AML evaluation -- deferred per TODO.md "AML
interpreter -- FUTURE"). The assertion validates that the ACPI write
path is wired correctly; full ACPI-driven power-off on every platform
requires the AML interpreter."""
import os, signal, socket, subprocess, sys, time

SER = "/tmp/stinkos_acpi_ser.log"
MON = "/tmp/stinkos_acpi_mon.sock"
SHELL_MENU_STEPS = 14   # presses of 's' to reach SHELL (slot 15 in menu)

for f in (SER, MON):
    try: os.remove(f)
    except OSError: pass

qemu = subprocess.Popen(
    ["qemu-system-i386", "-drive", "format=raw,file=os.bin", "-snapshot",
     "-display", "none", "-serial", "file:" + SER,
     "-monitor", "unix:%s,server,nowait" % MON, "-no-reboot"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def fail(msg):
    if qemu.poll() is None:
        qemu.send_signal(signal.SIGKILL)
    print("FAIL:", msg)
    try:
        with open(SER) as f: print("--- serial ---\n" + f.read())
    except OSError: pass
    sys.exit(1)


for _ in range(40):
    if os.path.exists(MON): break
    time.sleep(0.25)
else:
    fail("monitor socket never appeared")

sock = socket.socket(socket.AF_UNIX); sock.connect(MON); sock.recv(4096)

for _ in range(60):
    try:
        with open(SER) as f: data = f.read()
    except OSError: data = ""
    if "menu: ready" in data: break
    time.sleep(0.25)
else:
    fail("kernel did not reach menu: ready")

# Drive into SHELL and run `shutdown`.
for _ in range(SHELL_MENU_STEPS):
    sock.sendall(b"sendkey s\n"); time.sleep(0.05)
sock.sendall(b"sendkey ret\n"); time.sleep(2.0)


def shellkeys(text, pause=0.10):
    for ch in text:
        key = "spc" if ch == " " else ch.lower()
        sock.sendall(("sendkey %s\n" % key).encode())
        time.sleep(pause)


shellkeys("shutdown"); sock.sendall(b"sendkey ret\n")

for _ in range(30):
    if qemu.poll() is not None: break
    time.sleep(0.5)
else:
    fail("QEMU did not exit after shutdown command")

with open(SER) as f: out = f.read()

checks = {
    "acpi initialized":  "acpi: RSDP @" in out,
    "PM1a port cached":  "acpi: PM1a_CNT_BLK port=" in out,
    "acpi S5 fired":     "acpi: writing S5 to PM1a_CNT_BLK" in out,
    "qemu exited":       qemu.poll() is not None,
}
missing = [k for k, v in checks.items() if not v]
if missing:
    fail(", ".join(missing) + " not verified")

print("PASS: ACPI S5 write fired + QEMU powered off via shutdown command")
