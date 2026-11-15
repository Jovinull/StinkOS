#!/usr/bin/env python3
"""What the compositor costs, and that it does no work it does not have to.

The kernel prints one `win: perf` line for each second in which it drew a
frame (see kernel/sys/win.c). This script puts the desktop through five
situations and reads those lines:

  still screen            no frame at all
  pointer moving          frames, but nothing recomposed (only the pointer)
  still screen, a window  no frame at all
  dragging a window       only the area around the window is redrawn
  a program animating     one copy of its window per frame, not one per
                          window underneath

The pass/fail checks are about how much is drawn, which does not depend on
the host. The times are printed for information: they do.

    python3 tools/qa/wm_perf.py [--kvm]
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stinkqa import Vm, QaError, Desktop  # noqa: E402

SCREEN_KPX = 1024 * 768 // 1000
checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))
    print("%s %s%s" % ("ok  " if ok else "FAIL", name, (" -- " + detail) if detail and not ok else ""))


def perf(vm, mark):
    """The perf lines since `mark`, as dicts of ints."""
    return [dict((k, int(v)) for k, v in re.findall(r"(\w+)=(\d+)", line))
            for line in re.findall(r"win: perf (.*)", vm.serial()[mark:])]


def show(label, rows):
    frames = sum(r["frames"] for r in rows)
    if not frames:
        print("     %-28s no frames" % label)
        return
    print("     %-28s %3d frames, %5d us per frame on average, %5d us the longest" % (
        label, frames, sum(r["busy_us"] for r in rows) // frames, max(r["worst_us"] for r in rows)))


def main(argv):
    kvm = "--kvm" in argv
    with Vm(net=True, audio=True, cpu="host" if kvm else "Penryn",
            extra=["-enable-kvm"] if kvm else None) as vm:
        d = Desktop(vm)
        vm.mouse_to(100, 560)                    # below the tiles: nothing to hover
        time.sleep(2.5)

        mark = vm.mark()
        time.sleep(5)
        rows = perf(vm, mark)
        check("a still desktop draws no frames", not rows, str(rows))

        mark = vm.mark()
        for i in range(30):
            vm.mouse_to(100 + 25 * i, 540 + (i % 5) * 30)
        time.sleep(1.2)
        rows = perf(vm, mark)
        check("moving the pointer draws frames", sum(r["frames"] for r in rows) > 0)
        check("...and recomposes nothing", sum(r["composed"] for r in rows) == 0, str(rows))
        show("pointer moving", rows)

        pid = d.launch("rs-calc")
        g = d.geom(pid)
        vm.mouse_to(100, 560)
        time.sleep(2.5)
        mark = vm.mark()
        time.sleep(4)
        rows = perf(vm, mark)
        check("a still window draws no frames", not rows, str(rows))

        mark = vm.mark()
        vm.drag(g[0] + 60, g[1] - 12, g[0] + 360, g[1] + 200, steps=40)
        time.sleep(1.2)
        rows = perf(vm, mark)
        frames = sum(r["frames"] for r in rows)
        per_frame = sum(r["composed"] for r in rows) // max(frames, 1)
        win_kpx = (g[2] + 2) * (g[3] + 26) // 1000
        check("dragging redraws the window's neighbourhood, not the screen",
              frames > 0 and per_frame < 2 * win_kpx and per_frame < SCREEN_KPX // 2,
              "%d kpx per frame for a %d kpx window" % (per_frame, win_kpx))
        show("dragging a %dx%d window" % (g[2], g[3]), rows)
        mark = vm.mark()
        vm.key("esc")
        vm.wait_serial("win: destroy pid=%d" % pid, timeout=5, start=mark)

        pid = d.launch("rs-vec")
        g = d.geom(pid)
        vm.mouse_to(100, 560)
        time.sleep(2)
        mark = vm.mark()
        time.sleep(4)
        rows = perf(vm, mark)
        frames = sum(r["frames"] for r in rows)
        per_frame = sum(r["composed"] for r in rows) // max(frames, 1)
        check("an animating program costs its own window per frame",
              frames > 0 and per_frame <= g[2] * g[3] // 1000 + 1,
              "%d kpx per frame for a %d kpx window" % (per_frame, g[2] * g[3] // 1000))
        show("%dx%d animation" % (g[2], g[3]), rows)
        busy = sum(r["busy_us"] for r in rows)
        print("     compositor share of the CPU while it animates: %d %%" % (busy // (len(rows) * 10000)))

    failed = [c for c in checks if not c[1]]
    print("wm_perf: %d/%d checks passed" % (len(checks) - len(failed), len(checks)))
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except QaError as e:
        print("FAIL (harness): " + str(e))
        sys.exit(1)
