#!/usr/bin/env python3
"""W^X adversarial smoke. Drives QEMU shell to `run wxattack` which
writes a one-byte shellcode (0xC3 = ret) into a .data global and
function-pointer-calls it. v0.5 W^X stamps .data with NX so the CPU
takes a #PF on the first instruction fetch from .data and the kernel
kills the process. We assert both the "about to jump" line (proves
the app ran to the attack point) AND the resulting fault (proves the
W^X gate fired). Replaces the v0.5-era assertion that lived in
tools/test-headless.py."""
import os, signal, socket, subprocess, sys, time

SER = "/tmp/stinkos_wxa_ser.log"
MON = "/tmp/stinkos_wxa_mon.sock"
SHELL_MENU_STEPS = 14

for f in (SER, MON):
    try: os.remove(f)
    except OSError: pass

qemu = subprocess.Popen(
    # -cpu Penryn so TCG advertises NX (CPUID extended-leaf 1 EDX bit
    # 20); without it kentry's CPUID guard skips IA32_EFER.NXE and the
    # wxattack write to .data falls through to a real instruction fetch
    # of byte 0xC3 (ret) -- the test would false-pass.
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


shellkeys("run wxattack"); sock.sendall(b"sendkey ret\n")
time.sleep(3.0)

shellkeys("shutdown"); sock.sendall(b"sendkey ret\n")
for _ in range(30):
    if qemu.poll() is not None: break
    time.sleep(0.5)
else:
    qemu.send_signal(signal.SIGKILL)

with open(SER) as f: out = f.read()

checks = {
    "wxattack reached attack point":
        "wxattack: about to jump" in out,
    "shellcode did NOT execute":
        "wxattack: REACHED" not in out,
    "fault killed process":
        "app: fault, killed" in out,
}
missing = [k for k, v in checks.items() if not v]
if missing:
    fail(", ".join(missing))

print("PASS: W^X blocks .data execution via NX page fault on first instruction fetch")
