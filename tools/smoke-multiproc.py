#!/usr/bin/env python3
# Multi-process smoke for StinkOS.
# Boots the built image, navigates the graphical menu to SHELL, types
# `bg anim` to fork+exec ANIM in the background while the shell stays
# at its prompt, and asserts the serial log shows:
#   - shell launched
#   - exec: anim       (child took over with fork+exec)
#   - ring3: anim ...  (child reached ring 3 and ran)
#   - fs: wrote SHELL.HIS  (parent shell handled a follow-up command)
# Exits 0 on success, 1 on failure. Intentionally lightweight so it can
# run alongside `make test-headless` without doubling QEMU time.
import os, signal, socket, subprocess, sys, time

SER = "/tmp/stinkos_mp_ser.log"
MON = "/tmp/stinkos_mp_mon.sock"
SHELL_MENU_STEPS = 14   # presses of 's' to reach the SHELL row (slot 15)

for f in (SER, MON):
    try:
        os.remove(f)
    except OSError:
        pass

qemu = subprocess.Popen(
    ["qemu-system-i386", "-drive", "format=raw,file=os.bin", "-snapshot",
     "-display", "none", "-serial", "file:" + SER,
     "-monitor", "unix:%s,server,nowait" % MON,
     "-audiodev", "none,id=snd0", "-device", "sb16,audiodev=snd0",
     "-netdev", "user,id=net0", "-device", "e1000,netdev=net0",
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


# Connect to the QEMU monitor.
for _ in range(40):
    if os.path.exists(MON):
        break
    time.sleep(0.1)
else:
    fail("monitor socket never appeared")

sock = socket.socket(socket.AF_UNIX)
sock.connect(MON)
sock.settimeout(2.0)
time.sleep(0.3)
try:
    sock.recv(4096)
except (BlockingIOError, socket.timeout):
    pass


def send(cmd, pause=0.3):
    sock.sendall((cmd + "\n").encode())
    time.sleep(pause)
    try:
        while sock.recv(4096):
            pass
    except (BlockingIOError, socket.timeout):
        pass


def key(name, pause=0.18):
    send("sendkey " + name, pause)


def keys(text, pause=0.10):
    for c in text:
        key("spc" if c == " " else c.lower(), pause)


# Boot + DHCP settle.
time.sleep(14)

# Pick the SHELL slot in the menu, launch it.
for _ in range(SHELL_MENU_STEPS):
    key("s", pause=0.05)
key("ret", pause=0.0)
time.sleep(2.5)

# Type `bg anim` -- shell forks itself and execs ANIM in the child.
keys("bg anim")
key("ret", pause=0.6)
time.sleep(6.0)                          # let the child run a few SYS_LOG lines

# Follow up with a built-in shell command so we know the parent is
# still alive and able to dispatch new input after the fork.
keys("ps")
key("ret", pause=0.6)
time.sleep(3.5)                          # slow CI runners need extra slack for the 2nd SHELL.HIS write

send("quit", pause=0.5)

try:
    qemu.wait(timeout=8)
except subprocess.TimeoutExpired:
    qemu.send_signal(signal.SIGKILL)

out = serial()
checks = {
    "shell launched":    "loader: app loaded from fs" in out,
    "fork+exec child":   "exec: anim" in out,
    "child ring3 ran":   "ring3: anim app running" in out or
                         "ring3: anim: time flows" in out,
    "parent survived":   out.count("fs: wrote SHELL.HIS") >= 2,
    "no fault":          "app: fault" not in out,
}
missing = [name for name, ok in checks.items() if not ok]
if missing:
    fail(", ".join(missing) + " not observed")

print("PASS: shell fork+exec runs ANIM in the background; parent shell "
      "stays responsive and dispatches a follow-up command without fault")
