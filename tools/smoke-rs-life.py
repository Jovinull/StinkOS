#!/usr/bin/env python3
"""Conway's Game of Life smoke. Drives shell to `run rs-life`,
waits for the simulation to step a few hundred generations, sends
`q` to quit cleanly. Asserts the periodic gen=NNN alive=NNN log
lines appear AND that alive counts evolve over time (proves the
step() rule is actually firing -- not just a static grid)."""
import os, signal, socket, subprocess, sys, time, re

SER = "/tmp/stinkos_life_ser.log"
MON = "/tmp/stinkos_life_mon.sock"
SHELL_MENU_STEPS = 14

for f in (SER, MON):
    try: os.remove(f)
    except OSError: pass

qemu = subprocess.Popen(
    ["qemu-system-i386", "-cpu", "Penryn", "-snapshot",
     "-drive", "format=raw,file=os.bin", "-display", "none",
     "-serial", "file:" + SER,
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


shellkeys("run rs-life"); sock.sendall(b"sendkey ret\n")
# Let it run for ~25 seconds. With 10 gens/sec + log every 50 gens
# that's ~5 log lines emitted -- enough to assert evolution.
time.sleep(25.0)

# Quit cleanly.
sock.sendall(b"sendkey q\n"); time.sleep(2.0)

shellkeys("shutdown"); sock.sendall(b"sendkey ret\n")
for _ in range(30):
    if qemu.poll() is not None: break
    time.sleep(0.5)
else:
    qemu.send_signal(signal.SIGKILL)

with open(SER) as f: out = f.read()

if "life: start" not in out:
    fail("life: start missing")
if "life: bye" not in out:
    fail("life: bye missing (q didn't quit cleanly)")
if "app: fault, killed" in out:
    fail("rs-life faulted")

# Collect periodic gen log lines. They look like:
#   ring3: life: gen=50 alive=341
gen_re = re.compile(r'life: gen=(\d+) alive=(\d+)')
samples = [(int(g), int(a)) for g, a in gen_re.findall(out)]
if len(samples) < 2:
    fail(f"expected at least 2 gen log lines, got {len(samples)}: {samples}")

# Gens must monotonically increase.
gens = [g for g, _ in samples]
if gens != sorted(gens):
    fail(f"generation counter not monotonic: {gens}")

# Alive counts must vary -- a frozen grid would print the same number
# repeatedly (life on glider gun definitely varies as gliders spawn).
alives = [a for _, a in samples]
if len(set(alives)) == 1:
    fail(f"alive count never changed across {len(samples)} samples: {alives[0]}")

print(f"PASS: Conway's Life ran {gens[-1]} generations, alive count varied across "
      f"{len(set(alives))} distinct values")
