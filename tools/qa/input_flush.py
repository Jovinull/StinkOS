#!/usr/bin/env python3
"""Keys typed at one program must not be replayed into the next.

Boots straight into a test app that ignores its input for 2.5 s and then
exits to the kernel menu. While it sits there the harness types `s`, `s`,
Enter -- which the menu would read as "move down twice, launch". Passes
when the menu comes back and launches nothing.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stinkqa import Vm, fs_put  # noqa: E402


def main():
    img = Vm.copy_image("os.bin")
    fs_put(img, "AUTORUN", "KTEST\n")
    fs_put(img, "KTEST.ARG", "idle-exit\n")
    try:
        with Vm(image=img) as vm:
            vm.wait_serial("ktest: idle-exit: start", timeout=30)
            for k in ("s", "s", "ret"):
                vm.key(k)
            end = vm.wait_serial("menu: ready", timeout=15)
            time.sleep(1.0)                  # time for a stale Enter to do damage
            log = vm.serial()
    finally:
        os.remove(img)
    after = log[end:]
    launched = after.count("loader: app loaded from fs")
    if launched or "app: fault" in after or "menu: back" in after:
        print("FAIL: the menu acted on keys typed at the previous app "
              "(%d launch(es) after it came back)" % launched)
        return 1
    print("PASS: keys typed at the app were discarded when it exited")
    return 0


if __name__ == "__main__":
    sys.exit(main())
