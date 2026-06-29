#!/usr/bin/env python3
"""Drives QEMU into the shell, runs `cowtest`, scrapes serial for the
expected marker sequence. Validates that COW fork actually privatizes
the shared post-fork page on first write (parent and child diverge).
Exit 0 PASS, 1 FAIL."""
import os, signal, socket, subprocess, sys, time

SER = "/tmp/stinkos_cow_ser.log"
MON = "/tmp/stinkos_cow_mon.sock"
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


# `run cowtest` -- shell invokes sys_exec for the cowtest ELF, which
# runs to completion and exits back to the shell.
shellkeys("run cowtest"); sock.sendall(b"sendkey ret\n")
time.sleep(4.0)

# Shutdown cleanly so the qemu process exits and we can read the log.
shellkeys("shutdown"); sock.sendall(b"sendkey ret\n")
for _ in range(30):
    if qemu.poll() is not None: break
    time.sleep(0.5)
else:
    qemu.send_signal(signal.SIGKILL)

with open(SER) as f: out = f.read()

checks = {
    "pre-fork marker DEADBEEF":
        "cow: pre-fork marker=DEADBEEF" in out,
    "child saw shared pre-fork value":
        "cow: child saw pre=DEADBEEF wrote=CAFEBABE" in out,
    "parent saw shared pre-fork value":
        "cow: parent saw pre=DEADBEEF wrote=FEEDFACE" in out,
    "parent did not see child's write (COW privatized)":
        "cow: parent post-child read=FEEDFACE" in out,
    "no FAIL path taken":
        "cow: FAIL" not in out,
    "PASS marker present":
        "cow: PASS divergence" in out,
    "no kill-the-process fault during cowtest":
        # the wxattack and FAULT app faults happen in test-headless,
        # not this smoke target; cowtest must not fault.
        out.count("app: fault, killed") == 0,
}
missing = [k for k, v in checks.items() if not v]
if missing:
    fail(", ".join(missing))

print("PASS: COW fork privatizes shared pages on first write")
