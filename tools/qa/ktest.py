#!/usr/bin/env python3
"""Run the ktest kernel regression cases (apps/ktest.c), one VM per case.

    python3 tools/qa/ktest.py              # every case
    python3 tools/qa/ktest.py leak-fork    # just these
    python3 tools/qa/ktest.py --list

Each case boots a private copy of os.bin with two extra files: AUTORUN
(tells the kernel menu to start KTEST at boot) and KTEST.ARG (the case
name). The app logs its verdict on the serial port and powers the machine
off, so the harness waits on QEMU exiting instead of sleeping. A case that
hangs or takes the kernel down is reported as such and does not stop the
others. Exit status: 0 when every case passed.
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stinkqa import Vm, QaError, fs_put  # noqa: E402

CASES = [
    "leak-fork", "leak-exec", "pmm-churn", "heap-switch", "cow-kwrite",
    "text-kwrite", "munmap-hole", "kill-reap", "orphan", "procinfo", "sleep-fair",
    "preempt", "resched", "fair3", "fpu", "concurrent", "sbrk-oom", "oom", "eflags", "net-ping", "audio-unmap", "audio-modes", "pipes", "mount-guard", "badptr",
]
TIMEOUT = {"concurrent": 120, "badptr": 120}


def run_case(name, image="os.bin", mem=None, verbose=False):
    """Returns (status, detail): status is PASS, FAIL, CRASH or HANG."""
    img = Vm.copy_image(image)
    try:
        fs_put(img, "AUTORUN", "KTEST\n")
        fs_put(img, "KTEST.ARG", name + "\n")
        # NIC attached for every case: the network thread and its interrupt
        # are part of the kernel under test, not just of the net-ping case.
        with Vm(image=img, snapshot=True, mem=mem, net=True, audio=True) as vm:
            exited = vm.wait_exit(TIMEOUT.get(name, 60))
            log = vm.serial()
    finally:
        os.remove(img)
    if verbose:
        print(log)
    m = re.search(r"ktest: %s: (PASS|FAIL)(.*)" % re.escape(name), log)
    if m and exited:
        return m.group(1), m.group(2).strip()
    if "kernel exception" in log:
        tail = [l for l in log.splitlines() if "kernel exception" in l][-1]
        return "CRASH", tail.strip()
    if m:
        return m.group(1), m.group(2).strip() + " (VM did not power off)"
    last = log.strip().splitlines()[-1] if log.strip() else "(no serial output)"
    return "HANG", "no verdict; last line: " + last


def main(argv):
    verbose = "-v" in argv
    argv = [a for a in argv if a != "-v"]
    if "--list" in argv:
        print("\n".join(CASES))
        return 0
    mem = None
    if "--mem" in argv:
        i = argv.index("--mem")
        mem = argv[i + 1]
        del argv[i:i + 2]
    names = argv or CASES
    bad = 0
    for name in names:
        t0 = time.time()
        try:
            status, detail = run_case(name, mem=mem, verbose=verbose)
        except QaError as e:
            status, detail = "ERROR", str(e).splitlines()[0]
        if status != "PASS":
            bad += 1
        print("%-6s %-12s %5.1fs  %s" % (status, name, time.time() - t0, detail))
        sys.stdout.flush()
    print("ktest: %d/%d passed" % (len(names) - bad, len(names)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
