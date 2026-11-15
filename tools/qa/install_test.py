#!/usr/bin/env python3
"""The installer, end to end: clone the running system onto a blank second
disk, then boot that disk on its own.

  1. boot a private copy of os.bin with an empty disk as the secondary
     master, leave the desktop for the text menu, start INSTALLER, accept
     the full size and confirm;
  2. with the VM off, read the target from the host: it must carry a
     partition table and the same files;
  3. boot the target alone: the desktop must come up and open a window.

    python3 tools/qa/install_test.py
"""
import os
import struct
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stinkqa import Vm, QaError, Desktop, fs_list  # noqa: E402

TARGET_SIZE = 128 * 1024 * 1024
checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))
    print("%s %s%s" % ("ok  " if ok else "FAIL", name, (" -- " + detail) if detail and not ok else ""))


def main():
    src = Vm.copy_image("os.bin")
    fd, target = tempfile.mkstemp(prefix="stinkqa-target-", suffix=".bin")
    os.close(fd)
    with open(target, "r+b") as f:
        f.truncate(TARGET_SIZE)
    try:
        before = sorted(e[0] for e in fs_list(src))
        with Vm(image=src, snapshot=False, net=True, audio=True,
                extra=["-drive", "file=%s,format=raw,if=ide,index=2" % target]) as vm:
            Desktop(vm)
            mark = vm.mark()
            vm.click(900, 600)                   # the desktop takes the keyboard
            vm.key("ctrl", "q")
            vm.wait_serial("menu: ready", timeout=15, start=mark)
            elfs = [e[0] for e in fs_list(src) if e[0].endswith(".ELF")]
            for _ in elfs:
                vm.key("w", hold=20)
            for _ in range(elfs.index("INSTALLER.ELF")):
                vm.key("s", hold=20)
            mark = vm.mark()
            vm.key("ret")
            vm.wait_serial("loader: app loaded from fs", timeout=15, start=mark)
            time.sleep(2.0)                      # disk identification, size check
            vm.key("ret")                        # install size: the whole source
            time.sleep(1.0)
            vm.key("y")                          # overwrite the target
            vm.wait_serial("installer: wrote MBR partition table", timeout=600, start=mark)
            check("the installer ran to completion", True)
            vm.key("ret")                        # back to the menu
            vm.wait_serial("menu: back", timeout=15, start=mark)
            check("...and returned to the menu", True)

        with open(target, "rb") as f:
            mbr = f.read(512)
        boot, ptype, first, count = struct.unpack_from("<B3xB3xII", mbr, 446)
        check("the target has a boot signature and one active partition",
              mbr[510:512] == b"\x55\xaa" and boot == 0x80 and ptype == 0x7F and first == 1 and count > 0,
              "sig=%r boot=%#x type=%#x first=%d count=%d" % (mbr[510:512], boot, ptype, first, count))
        after = sorted(e[0] for e in fs_list(target))
        check("the target carries every file of the source, plus the first-boot marker",
              after == sorted(before + ["FIRSTBOOT.RUN"]),
              "missing %s, extra %s" % (sorted(set(before) - set(after)), sorted(set(after) - set(before))))
        check("the install media did not keep the first-boot marker",
              "FIRSTBOOT.RUN" not in [e[0] for e in fs_list(src)])

        with Vm(image=target, snapshot=True, net=True, audio=True) as vm:
            d = Desktop(vm)
            check("the installed disk boots to the desktop on its own", True)
            pid = d.launch("rs-about")
            mark = vm.mark()
            vm.key("esc")
            vm.wait_serial("win: destroy pid=%d" % pid, timeout=5, start=mark)
            check("...and runs a windowed program", True)
            log = vm.serial()
            trouble = [l for l in log.splitlines()
                       if "kernel exception" in l or "stack overflow" in l or "app: fault" in l]
            check("no kernel exception or fault on the installed system", not trouble, "; ".join(trouble[:3]))
    finally:
        for p in (src, target):
            if os.path.exists(p):
                os.remove(p)

    failed = [c for c in checks if not c[1]]
    print("install_test: %d/%d checks passed" % (len(checks) - len(failed), len(checks)))
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except QaError as e:
        print("FAIL (harness): " + str(e).splitlines()[0])
        print("\n".join(str(e).splitlines()[-12:]))
        sys.exit(1)
