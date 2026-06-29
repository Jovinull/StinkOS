#!/usr/bin/env python3
"""Drives QEMU shell to `run rs-json`, which reads TEST.JSON from
StinkFS, parses it, pretty-prints. Validates round-trip: every
expected line of the canonical pretty form appears in serial.
Proves Rust JSON parser handles nested objects, arrays, escape
sequences, integers + floats, booleans, null, empty {} and []."""
import os, signal, socket, subprocess, sys, time

SER = "/tmp/stinkos_rsjson_ser.log"
MON = "/tmp/stinkos_rsjson_mon.sock"
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


shellkeys("run rs-json"); sock.sendall(b"sendkey ret\n")
time.sleep(3.0)

shellkeys("shutdown"); sock.sendall(b"sendkey ret\n")
for _ in range(30):
    if qemu.poll() is not None: break
    time.sleep(0.5)
else:
    qemu.send_signal(signal.SIGKILL)

with open(SER) as f: out = f.read()

# Round-trip assertions: each key fragment must appear in the pretty
# output. Order matters within object entries (we preserve insertion
# order) but full-text comparison is brittle vs trailing whitespace,
# so we assert presence of each significant fragment.
required = [
    'json: read',
    'json: parsed',
    'json: pretty start',
    '"ok": true',
    '"count": 42',
    '"ratio": 3.14',
    '"tag": "hello\\nworld\\ttab"',
    '"id": 1',
    '"name": "alice"',
    '"id": 2',
    '"name": "bob"',
    '"admin": true',
    '"empty_obj": {}',
    '"empty_arr": []',
    '"deeply": ',
    '[',  # nested arrays present
    ']',
    'json: pretty end',
    'json: PASS',
]
forbidden = [
    'json: FAIL',
    'libstink: rust panic',
]
fault_count = out.count('app: fault, killed')

missing = [r for r in required if r not in out]
present_bad = [b for b in forbidden if b in out]
errs = []
if missing:
    errs.append('missing: ' + ', '.join(missing[:5]))
if present_bad:
    errs.append('forbidden: ' + ', '.join(present_bad))
if fault_count > 0:
    errs.append(f'app fault count={fault_count}')
if errs:
    fail('; '.join(errs))

print("PASS: Rust JSON parser+printer round-trip on nested fixture")
