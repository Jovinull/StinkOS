#!/usr/bin/env python3
"""Exercises the libstink Rust shim: println! with formatting,
eprintln!, alloc::format!. Asserts each formatted line appears on
serial in the right shape. Tier 2.2 milestone."""
import os, signal, socket, subprocess, sys, time

SER = "/tmp/stinkos_rsstdio_ser.log"
MON = "/tmp/stinkos_rsstdio_mon.sock"
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


shellkeys("run rs-stdio"); sock.sendall(b"sendkey ret\n")
time.sleep(3.0)

shellkeys("shutdown"); sock.sendall(b"sendkey ret\n")
for _ in range(30):
    if qemu.poll() is not None: break
    time.sleep(0.5)
else:
    qemu.send_signal(signal.SIGKILL)

with open(SER) as f: out = f.read()

checks = {
    "stdio started":         "rs-stdio: start" in out,
    "integer format":        "rs-stdio: integer=42" in out,
    "hex format":            "rs-stdio: hex=0xDEAD" in out,
    "multi-arg format":      "rs-stdio: multi a=1 b=two c=3.5" in out,
    "alloc::format roundtrip":
                             "rs-stdio: built=alloc-string-7" in out,
    "eprintln channel":      "rs-stdio: eprintln-channel" in out,
    "PASS marker":           "rs-stdio: PASS" in out,
    "no panic":              "libstink: rust panic" not in out,
    "no kill fault":         out.count("app: fault, killed") == 0,
}
missing = [k for k, v in checks.items() if not v]
if missing:
    fail(", ".join(missing))

print("PASS: libstink Rust shim println!/eprintln!/alloc::format! all work")
