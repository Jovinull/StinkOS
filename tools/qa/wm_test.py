#!/usr/bin/env python3
"""Window manager scenario: real pointer and keyboard input against the
booted desktop, checked through the screen and the serial log.

Covers: opening windows, focus and chrome colours, stacking and raise,
dragging (and that nothing is left behind where the window was), the
taskbar, key routing to the focused window only, the close box, a legacy
full-screen program taking and returning the screen, and that the desktop
is pixel-identical to its starting state once everything is closed.

    python3 tools/qa/wm_test.py [--shots DIR]

Exit 0 when every check passed. With --shots, screenshots of each step are
kept in DIR.
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stinkqa import Vm, QaError  # noqa: E402

# Colours from kernel/sys/win.c
TITLE_BG, TITLE_FOCUS = (0x16, 0x1b, 0x22), (0x1f, 0x29, 0x33)
BORDER, BORDER_FOCUS = (0x30, 0x36, 0x3d), (0x3d, 0x6b, 0x4f)
APP_BG = (0x0d, 0x11, 0x17)
BAR_BG = (0x0b, 0x0f, 0x14)
TITLE_H, BORDER_W, TASKBAR_H = 24, 1, 28
PARK = (1012, 700)                      # pointer rest position for screenshots

checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))
    print("%s %s%s" % ("ok  " if ok else "FAIL", name, (" -- " + detail) if detail and not ok else ""))


class Desk:
    def __init__(self, vm, shots):
        self.vm = vm
        self.shots = shots
        self.n = 0
        self.tiles = {}

    def read_tiles(self):
        for m in re.finditer(r"desktop: tile (\S+) (\d+) (\d+) (\d+) (\d+)", self.vm.serial()):
            x, y, w, h = map(int, m.groups()[1:])
            self.tiles[m.group(1)] = (x + w // 2, y + h // 2)

    def geom(self, pid):
        """Latest client rectangle of a window from the serial trace."""
        last = None
        for m in re.finditer(r"win: (show|moved|resize) pid=%d x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)" % pid,
                             self.vm.serial()):
            last = tuple(map(int, m.groups()[1:]))
        return last

    def launch(self, name):
        """Click a tile; returns the pid of the window that appears."""
        mark = self.vm.mark()
        self.vm.click(*self.tiles[name])
        end = self.vm.wait_serial("win: show pid=", timeout=15, start=mark)
        pid = int(re.match(r"(\d+)", self.vm.serial()[end:]).group(1))
        return pid

    def shot(self, label):
        self.vm.mouse_to(*PARK)
        img = self.vm.wait_stable(timeout=6)
        if self.shots:
            self.n += 1
            img.save(os.path.join(self.shots, "%02d-%s.png" % (self.n, label)))
        return img


def outer(g):
    x, y, w, h = g
    return (x - BORDER_W, y - TITLE_H - BORDER_W, x + w + BORDER_W, y + h + BORDER_W)


def inside(r, px, py):
    return r[0] <= px < r[2] and r[1] <= py < r[3]


def same_outside(a, b, holes):
    """Pixels that differ between two images, ignoring the `holes`."""
    wa, ha = a.size
    pa, pb = a.load(), b.load()
    bad = 0
    for y in range(ha):
        for x in range(wa):
            if pa[x, y] != pb[x, y] and not any(inside(h, x, y) for h in holes):
                bad += 1
    return bad


def main(argv):
    shots = None
    if "--shots" in argv:
        shots = argv[argv.index("--shots") + 1]
        os.makedirs(shots, exist_ok=True)

    with Vm(net=True, audio=True) as vm:
        vm.wait_serial("desktop: ready", timeout=40)       # window shown, tiles published
        d = Desk(vm, shots)
        d.read_tiles()
        check("desktop published its tiles", "rs-about" in d.tiles and "snake" in d.tiles)
        base = d.shot("desktop")
        clock = (1024 - 64, 768 - TASKBAR_H, 1024, 768)       # the minute may tick over

        # ---- one window --------------------------------------------------
        a = d.launch("rs-about")
        ga = d.geom(a)
        img = d.shot("one-window")
        check("window A has a focused title bar", img.getpixel((ga[0] + 40, ga[1] - 20)) == TITLE_FOCUS,
              str(img.getpixel((ga[0] + 40, ga[1] - 20))))
        check("window A has a focused border", img.getpixel((ga[0] - 1, ga[1] + 30)) == BORDER_FOCUS)
        check("window A client is drawn", img.getpixel((ga[0] + ga[2] - 3, ga[1] + 3)) == APP_BG)

        # ---- a second one on top ------------------------------------------
        b = d.launch("rs-about")
        gb = d.geom(b)
        img = d.shot("two-windows")
        check("windows cascade instead of stacking exactly", ga[:2] != gb[:2])
        check("opening B took focus from A", img.getpixel((ga[0] + 40, ga[1] - 20)) == TITLE_BG)
        check("B is focused", img.getpixel((gb[0] + 40, gb[1] - 20)) == TITLE_FOCUS)
        probe = (gb[0] + 200, gb[1] - 20)                     # B's title bar, inside A's client
        check("probe point lies in both windows", inside(outer(ga), *probe) and probe[1] > ga[1])
        check("B is drawn above A", img.getpixel(probe) == TITLE_FOCUS)

        # ---- raise A by clicking its title bar ----------------------------
        mark = vm.mark()
        vm.click(ga[0] + 60, ga[1] - 12)
        vm.wait_serial("win: focus pid=%d" % a, timeout=5, start=mark)
        img = d.shot("a-raised")
        check("clicking A raised it above B", img.getpixel(probe) == APP_BG, str(img.getpixel(probe)))
        check("A is focused again", img.getpixel((ga[0] + 40, ga[1] - 20)) == TITLE_FOCUS)

        # ---- taskbar brings B back ----------------------------------------
        mark = vm.mark()
        vm.click(84 + 136 + 4 + 60, 768 - TASKBAR_H // 2)     # second button
        vm.wait_serial("win: focus pid=%d" % b, timeout=5, start=mark)
        img = d.shot("taskbar-b")
        check("taskbar button focused and raised B", img.getpixel(probe) == TITLE_FOCUS)

        # ---- drag B -------------------------------------------------------
        mark = vm.mark()
        dx, dy = 230, 150
        vm.drag(gb[0] + 120, gb[1] - 12, gb[0] + 120 + dx, gb[1] - 12 + dy, steps=12)
        vm.wait_serial("win: moved pid=%d" % b, timeout=5, start=mark)
        gb2 = d.geom(b)
        check("B moved by the drag distance", (gb2[0] - gb[0], gb2[1] - gb[1]) == (dx, dy),
              "moved %s" % ((gb2[0] - gb[0], gb2[1] - gb[1]),))
        img = d.shot("b-dragged")
        # Where B used to be and neither window is now must show the desktop
        # exactly as it was before any window existed.
        ob, ob2, oa = outer(gb), outer(gb2), outer(ga)
        px_img, px_base = img.load(), base.load()
        ghost = 0
        for y in range(ob[1], ob[3]):
            for x in range(ob[0], ob[2]):
                if inside(ob2, x, y) or inside(oa, x, y) or y < 64:
                    continue
                if px_img[x, y] != px_base[x, y]:
                    ghost += 1
        check("no pixels left behind where B was", ghost == 0, "%d stale pixels" % ghost)

        # ---- keys go to the focused window only ---------------------------
        exits = vm.serial_count("rs-about: exit")
        mark = vm.mark()
        vm.key("esc")
        vm.wait_serial("win: destroy pid=%d" % b, timeout=5, start=mark)
        time.sleep(0.5)
        check("Esc closed exactly one window", vm.serial_count("rs-about: exit") == exits + 1)
        check("the unfocused window did not get the key",
              "win: destroy pid=%d" % a not in vm.serial()[mark:])

        # ---- close box ----------------------------------------------------
        mark = vm.mark()
        vm.click(ga[0] + ga[2] - 12, ga[1] - 12)
        vm.wait_serial("win: destroy pid=%d" % a, timeout=5, start=mark)
        time.sleep(0.6)
        img = d.shot("all-closed")
        left = same_outside(img, base, [clock])
        check("desktop is pixel-identical after closing everything", left == 0,
              "%d pixels differ" % left)

        # ---- a legacy full-screen program ---------------------------------
        mark = vm.mark()
        vm.click(*d.tiles["snake"])
        vm.wait_serial("win: fullscreen pid=", timeout=15, start=mark)
        time.sleep(1.0)
        fs = vm.image()
        if shots:
            fs.save(os.path.join(shots, "20-fullscreen.png"))
        check("compositor leaves the screen to a full-screen program",
              fs.getpixel((40, 768 - 10)) != BAR_BG, str(fs.getpixel((40, 768 - 10))))
        vm.key("q")
        vm.wait_serial("win: fullscreen-end", timeout=10, start=mark)
        time.sleep(0.6)
        img = d.shot("after-fullscreen")
        left = same_outside(img, base, [clock])
        check("desktop is restored after the full-screen program exits", left == 0,
              "%d pixels differ" % left)

        # ---- open / close churn -------------------------------------------
        for i in range(6):
            pid = d.launch("rs-about")
            mark = vm.mark()
            vm.key("esc")
            vm.wait_serial("win: destroy pid=%d" % pid, timeout=5, start=mark)
        time.sleep(0.6)
        img = d.shot("after-churn")
        left = same_outside(img, base, [clock])
        check("desktop is clean after 6 open/close cycles", left == 0, "%d pixels differ" % left)

        log = vm.serial()
        trouble = [l for l in log.splitlines()
                   if "kernel exception" in l or "stack overflow" in l
                   or "bytes of kernel stack" in l or "app: fault" in l]
        check("no kernel exception, fault or stack warning", not trouble, "; ".join(trouble[:3]))

    failed = [c for c in checks if not c[1]]
    print("wm_test: %d/%d checks passed" % (len(checks) - len(failed), len(checks)))
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except QaError as e:
        print("FAIL (harness): " + str(e))
        sys.exit(1)
