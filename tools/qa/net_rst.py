#!/usr/bin/env python3
"""Network abuse smoke: unsolicited TCP from the host while an app runs.

Boots with a ring-3 app in the foreground (so interrupts and syscalls use
the process's 8 KiB kernel stack, not the roomy early boot stack), then
opens TCP connections from the host to a guest port nobody listens on.
Every one makes the kernel answer with a RST. Passes when the guest stays
alive, keeps answering, and its kernel stack audit stays quiet.
"""
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stinkqa import Vm, fs_put  # noqa: E402


def main():
    img = Vm.copy_image("os.bin")
    fs_put(img, "AUTORUN", "ANIM\n")
    port = 20000 + os.getpid() % 20000
    resets = 0
    try:
        with Vm(image=img, net=True, hostfwd=[(port, 7777)]) as vm:
            vm.wait_serial("anim app running", timeout=30)
            vm.wait_serial("dhcp: bound", timeout=20)
            for _ in range(40):
                s = socket.socket()
                s.settimeout(2)
                try:
                    s.connect(("127.0.0.1", port))
                    s.sendall(b"x" * 64)
                    s.recv(16)
                except ConnectionResetError:
                    resets += 1
                except OSError:
                    pass
                s.close()
            time.sleep(1.5)                      # let a stack audit tick pass
            mark = vm.mark()
            vm.key("a")
            try:
                vm.wait_serial("kbd: a", timeout=5, start=mark)
                alive = True
            except Exception:
                alive = False
            log = vm.serial()
    finally:
        os.remove(img)

    problems = [l for l in log.splitlines()
                if "kernel exception" in l or "stack overflow" in l
                or "bytes of kernel stack" in l]
    if not alive:
        problems.append("guest stopped responding to the keyboard")
    if resets < 30:
        problems.append("only %d of 40 connections were reset by the guest" % resets)
    if problems:
        print("FAIL: " + "; ".join(problems))
        return 1
    print("PASS: 40 unsolicited connections reset (%d RSTs seen), kernel stack quiet" % resets)
    return 0


if __name__ == "__main__":
    sys.exit(main())
