#!/usr/bin/env python3
"""Keyboard modifier state, driven key by key from the host.

The kernel logs every decoded character on serial ("kbd: X"), which is
what this reads. Covers the cases that used to go wrong:
  - both Shifts held, one released: still shifted
  - Caps Lock held down (auto-repeating make codes): toggles once
  - the Pause key's E1 sequence must not act as Ctrl
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stinkqa import Vm, fs_remove  # noqa: E402


def typed(vm, mark):
    """Characters the kernel decoded since `mark`."""
    out = []
    for line in vm.serial()[mark:].splitlines():
        if line.startswith("kbd: ") and len(line) == 6:
            out.append(line[5])
    return "".join(out)


def main():
    img = Vm.copy_image("os.bin")
    fs_remove(img, "AUTORUN")                    # plain text menu: it just consumes keys
    problems = []
    try:
        with Vm(image=img) as vm:
            vm.wait_serial("menu: ready", timeout=30)

            def expect(label, want, keys):
                mark = vm.mark()
                keys()
                time.sleep(0.4)
                got = typed(vm, mark)
                ok = got == want
                print("%s %s (typed %r, want %r)" % ("ok  " if ok else "FAIL", label, got, want))
                if not ok:
                    problems.append(label)

            def two_shifts():
                vm.key_down("shift"); vm.key_down("shift_r")
                vm.key("x")
                vm.key_up("shift_r")                 # left one is still held
                vm.key("x")
                vm.key_up("shift")
                vm.key("x")
            expect("releasing one Shift keeps the other in effect", "XXx", two_shifts)

            def caps_repeat():
                for _ in range(5):                   # make code repeated, no release
                    vm.key_down("caps_lock"); time.sleep(0.03)
                vm.key_up("caps_lock")
                vm.key("x")
                vm.key("caps_lock")                  # a clean press turns it off again
                vm.key("x")
            expect("held Caps Lock toggles once", "Xx", caps_repeat)

            def pause_key():
                vm.key("pause")
                vm.key("x")                          # Ctrl+x would be dropped or a control code
            expect("Pause does not leave Ctrl stuck", "x", pause_key)
    finally:
        os.remove(img)
    if problems:
        print("FAIL: " + "; ".join(problems))
        return 1
    print("PASS: keyboard modifier state")
    return 0


if __name__ == "__main__":
    sys.exit(main())
