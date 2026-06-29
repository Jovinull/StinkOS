#!/usr/bin/env python3
"""Drives QEMU into the shell, runs `mounttest`, scrapes serial for the
expected mount + write + read + delete sequence. Validates that
sys_mount registers a second StinkFS slot AND that the fs prefix
resolver routes B:hello to it. Exit 0 PASS, 1 FAIL."""
import os, signal, socket, subprocess, sys, time

SER = "/tmp/stinkos_vfs_ser.log"
MON = "/tmp/stinkos_vfs_mon.sock"
SHELL_MENU_STEPS = 14

for f in (SER, MON):
    try: os.remove(f)
    except OSError: pass

qemu = subprocess.Popen(
    ["qemu-system-i386", "-cpu", "Penryn",
     "-drive", "format=raw,file=os.bin", "-snapshot",
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

# Open SHELL.
for _ in range(SHELL_MENU_STEPS):
    sock.sendall(b"sendkey s\n"); time.sleep(0.05)
sock.sendall(b"sendkey ret\n"); time.sleep(2.0)


def shellkeys(text, pause=0.10):
    for ch in text:
        key = "spc" if ch == " " else ch.lower()
        sock.sendall(("sendkey %s\n" % key).encode())
        time.sleep(pause)


# Run the mount test app.
shellkeys("run mounttest"); sock.sendall(b"sendkey ret\n")
time.sleep(3.0)

# Clean shutdown to flush serial + let QEMU exit.
shellkeys("shutdown"); sock.sendall(b"sendkey ret\n")
for _ in range(30):
    if qemu.poll() is not None: break
    time.sleep(0.5)
else:
    qemu.send_signal(signal.SIGKILL)

with open(SER) as f: out = f.read()

checks = {
    "sys_mount(1) ok":         "mount: sys_mount(1) ok" in out,
    "wrote B:hello":           "mount: wrote B:hello bytes=" in out,
    "read B:hello":            "mount: read B:hello bytes=" in out,
    "marker DEADBEEF":         "marker=DEADBEEF" in out,
    "no FAIL path taken":      "mount: FAIL" not in out,
    "PASS marker present":     "mount: PASS roundtrip" in out,
    "no kill-the-process fault during mounttest":
        out.count("app: fault, killed") == 0,
}
missing = [k for k, v in checks.items() if not v]
if missing:
    fail(", ".join(missing))

print("PASS: sys_mount + fs prefix resolver round-trip a file on slot B:")
