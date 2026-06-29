#!/usr/bin/env python3
"""Drives QEMU into the shell, runs `rs-hello`, asserts the Rust
extern "C" main() actually executed sys_log via the libstink_syms
shims. First milestone of the Rust userland (Tier 1.1)."""
import os, signal, socket, subprocess, sys, time

SER = "/tmp/stinkos_rs_ser.log"
MON = "/tmp/stinkos_rs_mon.sock"
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
    if qemu.poll() is None: qemu.send_signal(signal.SIGKILL)
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

for _ in range(SHELL_MENU_STEPS):
    sock.sendall(b"sendkey s\n"); time.sleep(0.05)
sock.sendall(b"sendkey ret\n"); time.sleep(2.0)


def shellkeys(text, pause=0.10):
    aliases = {" ": "spc", "-": "minus", ".": "dot"}
    for ch in text:
        key = aliases.get(ch, ch.lower())
        sock.sendall(("sendkey %s\n" % key).encode())
        time.sleep(pause)


shellkeys("run rs-hello"); sock.sendall(b"sendkey ret\n")
time.sleep(2.0)

shellkeys("shutdown"); sock.sendall(b"sendkey ret\n")
for _ in range(30):
    if qemu.poll() is not None: break
    time.sleep(0.5)
else:
    qemu.send_signal(signal.SIGKILL)

with open(SER) as f: out = f.read()

checks = {
    "rs-hello loaded":   "loader: app loaded from fs" in out,
    "rust main ran":     "rs-hello: hi from rust" in out,
    "no rust panic":     "rs-hello: rust panic" not in out,
    "no kill fault on rs-hello":
        # Existing test-headless triggers one expected fault from the
        # FAULT app; we run a clean QEMU instance so the count starts
        # at 0 and any fault here would be from rs-hello.
        out.count("app: fault, killed") == 0,
}
missing = [k for k, v in checks.items() if not v]
if missing:
    fail(", ".join(missing))

print("PASS: Rust app rs-hello called sys_log via extern C bindings")
