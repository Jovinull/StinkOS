#!/usr/bin/env python3
"""Every bundled windowed program does what it says.

Drives each program on the booted desktop with real input and checks the
result two ways: by reading the text it draws (the UI font is a bitmap, so
screenshots are read back exactly) and, for programs that save files, by
inspecting the disk image from the host after the VM has shut down and
again after a reboot.

    python3 tools/qa/apps_test.py [--shots DIR]

The image used is a private writable copy; os.bin is not touched.
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stinkqa import (Vm, QaError, Desktop, fs_read, fs_list)  # noqa: E402

checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok)))
    print("%s %s%s" % ("ok  " if ok else "FAIL", name, (" -- " + str(detail)) if detail and not ok else ""))


def close(vm, d, pid, key="esc"):
    mark = vm.mark()
    vm.key(key)
    try:
        d.wait_closed(pid, start=mark, timeout=4)
    except QaError:
        vm.key(key)                              # e.g. calculator: first Esc clears
        d.wait_closed(pid, start=mark, timeout=4)


def session_one(img, shots):
    with Vm(image=img, snapshot=False, net=True, audio=True) as vm:
        d = Desktop(vm)
        base = d.shot()

        def keep(label):
            if shots:
                d.shot(settle=False).save(os.path.join(shots, label + ".png"))

        # ---- calculator --------------------------------------------------
        p = d.launch("rs-calc")
        g = d.geom(p)
        display = lambda: d.text(p).split("\n")[0].split()[-1]
        vm.type("12+34\n")
        check("calc: 12 + 34 = 46", display() == "46", d.text(p)[:40])
        vm.type("c11*11\n")
        check("calc: same digit twice in a row (11 * 11 = 121)", display() == "121", d.text(p)[:40])
        vm.type("c2+3*4\n")
        check("calc: chained operations left to right (2 + 3 * 4 = 20)", display() == "20", d.text(p)[:40])
        vm.type("c5/0\n")
        check("calc: division by zero is reported", "zero" in d.text(p).lower(), d.text(p)[:60])
        vm.type("c")
        for label in ("7", "*", "6", "="):       # the same with the mouse
            d.click_text(p, label)
        check("calc: buttons work with the mouse (7 * 6 = 42)", display() == "42", d.text(p)[:40])
        keep("calc")
        close(vm, d, p)

        # ---- todo --------------------------------------------------------
        p = d.launch("rs-todo")
        vm.type("a"); vm.type("buy milk\n")
        vm.type("a"); vm.type("write tests\n")
        vm.key("up"); vm.key("up"); vm.key("spc")
        t = d.text(p)
        check("todo: items appear in the list", "buy milk" in t and "write tests" in t, t[:80])
        check("todo: toggling marks an item done", "1/2 done" in t, t[:80])
        keep("todo")
        close(vm, d, p, key="q")

        # ---- editor: type, save -----------------------------------------
        p = d.launch("rs-edit")
        vm.type("n")
        vm.type("hello world\nsecond line 123")
        t = d.text(p)
        check("edit: typed text is shown", "hello world" in t and "second line 123" in t, t[:80])
        check("edit: unsaved changes are flagged", "[modified]" in t)
        vm.key("ctrl", "s")
        time.sleep(0.5)
        t = d.text(p)
        check("edit: saving clears the modified flag", "[modified]" not in t, t[-120:])
        name = re.search(r"(NOTES\d*\.TXT)", t)
        check("edit: the status bar names the file", bool(name), t[-120:])
        edit_name = name.group(1) if name else "NOTES.TXT"

        # ---- resize the editor ------------------------------------------
        g = d.geom(p)
        mark = vm.mark()
        vm.drag(g[0] + g[2] - 4, g[1] + g[3] - 4, g[0] + g[2] - 4 + 100, g[1] + g[3] - 4 + 60, steps=8)
        try:
            vm.wait_serial("win: resize pid=%d" % p, timeout=5, start=mark)
            g2 = d.geom(p)
            check("edit: dragging the corner resizes the window",
                  abs(g2[2] - g[2] - 100) <= 8 and abs(g2[3] - g[3] - 60) <= 8,
                  "from %s to %s" % (g[2:], g2[2:]))
            t = d.text(p)
            check("edit: content survives the resize", "hello world" in t and "second line 123" in t, t[:80])
        except QaError as e:
            check("edit: dragging the corner resizes the window", False, str(e).splitlines()[0])
        keep("edit")
        close(vm, d, p)

        # ---- files -> editor --------------------------------------------
        p = d.launch("rs-files")
        t = d.text(p)
        check("files: lists the files on the disk", "HELLO.ELF" in t and "files" in t, t[:80])
        vm.key("end")
        t = d.text(p)
        check("files: End selects the last file", edit_name in t, t[-200:])
        mark = vm.mark()
        vm.key("ret")
        try:
            end = vm.wait_serial("win: show pid=", timeout=10, start=mark)
            ep = int(re.match(r"(\d+)", vm.serial()[end:]).group(1))
            time.sleep(0.6)
            t = d.text(ep)
            # the cursor underline sits on the first character, so read from the second
            check("files: Enter opens a text file in the editor", "ello world" in t, t[:80])
            keep("files-edit")
            close(vm, d, ep)
        except QaError as e:
            check("files: Enter opens a text file in the editor", False, str(e).splitlines()[0])
        close(vm, d, p)

        # ---- paint -------------------------------------------------------
        p = d.launch("rs-paint")
        g = d.geom(p)
        vm.drag(g[0] + 100, g[1] + 150, g[0] + 400, g[1] + 300, steps=10)
        mark = vm.mark()
        vm.key("ctrl", "s")
        vm.wait_serial("fs: wrote CANVAS.BIN", timeout=30, start=mark)
        t = d.text(p)
        check("paint: save is confirmed", "saved" in t.lower(), t[-120:])
        keep("paint")
        close(vm, d, p)

        # ---- hex ---------------------------------------------------------
        p = d.launch("rs-hex")
        t = d.text(p)
        check("hex: shows the first file from offset 0", "00000000" in t and "7f 45 4c 46" in t, t[:120])
        close(vm, d, p, key="q")

        # ---- clock / calendar / about -----------------------------------
        p = d.launch("rs-clock")
        t = d.text(p)
        check("clock: shows today's date", re.search(r"\d{4}-\d{2}-\d{2}", t), t)
        close(vm, d, p)
        p = d.launch("rs-cal")
        t = d.text(p)
        check("calendar: shows a month grid", "Su" in t and "Mo" in t and re.search(r"\b20\d\d\b", t), t[:80])
        vm.key("right")
        t2 = d.text(p)
        check("calendar: Right changes the month", t2.split("\n")[0] != t.split("\n")[0], t2[:40])
        close(vm, d, p)
        p = d.launch("rs-about")
        check("about: opens", "StinkOS" in d.text(p))
        close(vm, d, p)

        # ---- sysinfo / net ----------------------------------------------
        p = d.launch("rs-sysinfo")
        t = d.text(p)
        check("sysinfo: memory and process table", "MiB used" in t and "RS-SYSINFO" in t, t[:200])
        close(vm, d, p)
        p = d.launch("rs-net")
        time.sleep(1.2)
        t = d.text(p)
        check("net: reports the DHCP lease", "bound" in t and "10.0.2.15" in t, t[:200])
        close(vm, d, p)

        # ---- settings ----------------------------------------------------
        p = d.launch("rs-settings")
        d.click_text(p, "BR (ABNT2)")
        time.sleep(0.3)
        t = d.text(p)
        active = [l for l in t.split("\n") if "Active layout" in l]
        check("settings: clicking BR switches the keyboard layout", active and "BR" in active[0], t[:160])
        d.click_text(p, "US (QWERTY)")
        time.sleep(0.3)
        active = [l for l in d.text(p).split("\n") if "Active layout" in l]
        check("settings: and US switches it back", active and "US" in active[0])
        close(vm, d, p)

        # ---- vector art animates ----------------------------------------
        p = d.launch("rs-vec")
        a = vm.image(); time.sleep(0.6); b = vm.image()
        check("vec: the figure animates", a.tobytes() != b.tobytes())
        # Resize it four times while it is drawing: bigger, smaller, wide, tall.
        sizes_ok, g = True, d.geom(p)
        for dw, dh in ((120, 80), (-200, -150), (260, -40), (-180, 190)):
            mark = vm.mark()
            vm.drag(g[0] + g[2] - 4, g[1] + g[3] - 4, g[0] + g[2] - 4 + dw, g[1] + g[3] - 4 + dh, steps=6)
            try:
                vm.wait_serial("win: resize pid=%d" % p, timeout=5, start=mark)
            except QaError:
                sizes_ok = False
                break
            g2 = d.geom(p)
            sizes_ok = sizes_ok and abs(g2[2] - g[2] - dw) <= 8 and abs(g2[3] - g[3] - dh) <= 8
            g = g2
        check("vec: four resizes in a row while it animates", sizes_ok, str(g))
        time.sleep(0.5)
        a = vm.image().crop((g[0], g[1], g[0] + g[2], g[1] + g[3]))
        time.sleep(0.5)
        b = vm.image().crop((g[0], g[1], g[0] + g[2], g[1] + g[3]))
        check("vec: still animating at the last size, edge to edge",
              a.tobytes() != b.tobytes() and a.getpixel((g[2] - 2, 2)) == b.getpixel((g[2] - 2, 2)))
        close(vm, d, p)

        # ---- task manager kills a program -------------------------------
        # Task manager first: its window leaves the About tile uncovered.
        tm = d.launch("rs-taskman")
        victim = d.launch("rs-about")
        gt = d.geom(tm)
        mark = vm.mark()
        vm.click(gt[0] + 60, gt[1] - 12)         # its title bar: focus and raise it again
        vm.wait_serial("win: focus pid=%d" % tm, timeout=5, start=mark)
        time.sleep(2.2)                          # one auto-refresh, so the new process is listed
        t = d.text(tm)
        rows = [l for l in t.split("\n") if re.match(r"\d+ \w+ \d+ \d+ ", l)]
        check("taskman: lists the running programs by name",
              any("RS-ABOUT" in r for r in rows) and any("RS-TASKMAN" in r for r in rows), t[:300])
        idx = next((i for i, r in enumerate(rows) if r.startswith("%d " % victim)), None)
        if idx is not None:
            for _ in range(idx):
                vm.key("down")
            mark = vm.mark()
            vm.type("k")
            try:
                d.wait_closed(victim, start=mark, timeout=6)
                check("taskman: K kills the selected program", True)
            except QaError:
                check("taskman: K kills the selected program", False, d.text(tm)[-200:])
        else:
            check("taskman: K kills the selected program", False, "victim row not found")
        close(vm, d, tm)

        # ---- everything is gone -----------------------------------------
        time.sleep(1.0)
        final = d.shot()
        t = "\n".join(s for s in [__import__("stinkqa").screen_text(final, (700, 10, 1024, 50),
                                                                    __import__("stinkqa").UI_TEXT)])
        check("desktop: no program left running", "0 programs running" in t, t)
        log = vm.serial()
        trouble = [l for l in log.splitlines()
                   if "kernel exception" in l or "stack overflow" in l
                   or "bytes of kernel stack" in l or "app: fault" in l or "OUT OF MEMORY" in l]
        check("no kernel exception, fault, stack warning or OOM", not trouble, "; ".join(trouble[:3]))
        return edit_name


def session_two(img, edit_name):
    """After a reboot: what was saved is still there."""
    with Vm(image=img, snapshot=True, net=True, audio=True) as vm:
        d = Desktop(vm)
        p = d.launch("rs-todo")
        t = d.text(p)
        check("todo: the list survives a reboot", "buy milk" in t and "write tests" in t and "1/2 done" in t, t[:80])
        close(vm, d, p, key="q")
        p = d.launch("rs-paint")
        vm.key("ctrl", "o")
        time.sleep(1.5)
        g = d.geom(p)
        img_now = d.shot()
        # the stroke drawn in the first session ran through (250, 225) of the client area
        px = img_now.getpixel((g[0] + 250, g[1] + 225))
        check("paint: Ctrl+O brings the saved drawing back", px == (0, 0, 0), str(px))
        close(vm, d, p)


def main(argv):
    shots = None
    if "--shots" in argv:
        shots = argv[argv.index("--shots") + 1]
        os.makedirs(shots, exist_ok=True)
    img = Vm.copy_image("os.bin")
    try:
        edit_name = session_one(img, shots)

        # ---- the disk, from the host -------------------------------------
        names = [n for n, _, _ in fs_list(img)]
        todo = fs_read(img, "TODO.DAT")
        check("disk: TODO.DAT holds the two items", todo is not None and
              sorted(todo.decode(errors="replace").lower().splitlines()) ==
              sorted(["[x] buy milk", "[ ] write tests"]), repr(todo))
        note = fs_read(img, edit_name)
        check("disk: the editor saved exactly what was typed",
              note == b"hello world\nsecond line 123", repr(note))
        canvas = fs_read(img, "CANVAS.BIN")
        check("disk: CANVAS.BIN is a canvas with ink on it",
              canvas is not None and canvas[:4] == b"SPNT" and len(canvas) == 384008
              and any(canvas[8:]), "len=%s" % (len(canvas) if canvas else None))
        check("disk: no stray LAUNCH.TMP left behind", "LAUNCH.TMP" not in names)

        session_two(img, edit_name)
    finally:
        os.remove(img)

    failed = [c for c in checks if not c[1]]
    print("apps_test: %d/%d checks passed" % (len(checks) - len(failed), len(checks)))
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except QaError as e:
        print("FAIL (harness): " + str(e))
        sys.exit(1)
