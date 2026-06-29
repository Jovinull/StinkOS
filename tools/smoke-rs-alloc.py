#!/usr/bin/env python3
"""Drives QEMU into the shell, runs `rs-alloc`, asserts that
Rust's `alloc` crate (Box / Vec / re-allocation) works through our
libstink K&R malloc backing via #[global_allocator]. Tier 1.3
milestone."""
import os, signal, socket, subprocess, sys, time

SER = "/tmp/stinkos_rsalloc_ser.log"
MON = "/tmp/stinkos_rsalloc_mon.sock"
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


shellkeys("run rs-alloc"); sock.sendall(b"sendkey ret\n")
time.sleep(3.0)

shellkeys("shutdown"); sock.sendall(b"sendkey ret\n")
for _ in range(30):
    if qemu.poll() is not None: break
    time.sleep(0.5)
else:
    qemu.send_signal(signal.SIGKILL)

with open(SER) as f: out = f.read()

checks = {
    "alloctest started":  "rs-alloc: start" in out,
    "Box<u32> ok":        "rs-alloc: box ok" in out,
    "Vec sum=496 ok":     "rs-alloc: vec ok sum=496" in out,
    "free + reuse ok":    "rs-alloc: free reuse ok" in out,
    "no rust panic":      "rs-alloc: rust panic" not in out,
    "no FAIL path":       "rs-alloc: FAIL" not in out,
    "PASS marker":        "rs-alloc: PASS" in out,
    "no kill fault":      out.count("app: fault, killed") == 0,
}
missing = [k for k, v in checks.items() if not v]
if missing:
    fail(", ".join(missing))

print("PASS: Rust alloc crate (Box + Vec + free/reuse) through libstink malloc")
