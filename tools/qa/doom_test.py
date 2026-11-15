#!/usr/bin/env python3
"""Doom, end to end, started from the desktop.

Covers what the port supports today: start-up and WAD loading, rendering,
keyboard input (press and release), the HUD reacting to play, saving a
game with a typed name, quitting through the menu back to an intact
desktop, starting it a second time, and loading the saved game from disk.
Sound effects are exercised (an SB16 is attached) but cannot be heard by a
script; music is not implemented in the port.

    python3 tools/qa/doom_test.py [--shots DIR]

Needs FREEDOOM1.WAD in the image (tools/fetch-wads.sh). Uses a private
writable copy of os.bin.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stinkqa import Vm, QaError, Desktop, fs_list, fs_read  # noqa: E402

checks = []
VIEW = (192, 184, 832, 584)             # the 640x400 picture, centred on 1024x768
HUD = (192, 520, 832, 584)              # status bar: the bottom 32 Doom rows, doubled


def check(name, ok, detail=""):
    checks.append((name, bool(ok)))
    print("%s %s%s" % ("ok  " if ok else "FAIL", name, (" -- " + str(detail)) if detail and not ok else ""))


def colours(img, box):
    c = img.crop(box)
    return len(c.getcolors(c.size[0] * c.size[1]))


def differs(a, b, box):
    return a.crop(box).tobytes() != b.crop(box).tobytes()


def start_doom(vm, d):
    mark = vm.mark()
    vm.click(*d.tiles["doom1"])
    vm.wait_serial("win: fullscreen pid=", timeout=90, start=mark)
    vm.wait_serial("I_InitGraphics: Auto-scaling factor", timeout=90, start=mark)
    time.sleep(4.0)                     # title picture is up
    return mark


def quit_doom(vm, mark):
    vm.key("f10"); time.sleep(0.8)      # Quit Game, wherever the menu cursor was left
    vm.key("y")
    vm.wait_serial("win: fullscreen-end", timeout=20, start=mark)


def main(argv):
    shots = None
    if "--shots" in argv:
        shots = argv[argv.index("--shots") + 1]
        os.makedirs(shots, exist_ok=True)

    def keep(img, name):
        if shots:
            img.save(os.path.join(shots, name + ".png"))

    img = Vm.copy_image("os.bin")
    try:
        if fs_read(img, "FREEDOOM1.WAD") is None:
            print("SKIP: no FREEDOOM1.WAD in os.bin (run tools/fetch-wads.sh and rebuild)")
            return 0
        before = set(n for n, _, _ in fs_list(img))

        with Vm(image=img, snapshot=False, net=True, audio=True) as vm:
            d = Desktop(vm)
            base = d.shot()
            clock = (960, 740, 1024, 768)

            mark = start_doom(vm, d)
            title = vm.image(); keep(title, "01-title")
            check("title screen is drawn", colours(title, VIEW) > 50, colours(title, VIEW))
            check("the compositor is out of the way (no taskbar)",
                  title.getpixel((40, 760)) == (0, 0, 0), title.getpixel((40, 760)))

            # New game: Enter (menu), Enter (New Game), Enter (episode), Enter (skill)
            for _ in range(4):
                vm.key("ret"); time.sleep(0.8)
            time.sleep(4.0)
            play = vm.image(); keep(play, "02-play")
            check("in game: a status bar with content", colours(play, HUD) > 30, colours(play, HUD))

            # Hold a turn key: the view must sweep while the key is down and
            # settle once it is released (the key-up has to reach the game).
            # Lights flicker and the weapon bobs even when standing still, so
            # compare coarse structure -- a 40x21 greyscale thumbnail -- not
            # exact pixels.
            view_only = (VIEW[0], VIEW[1], VIEW[2], HUD[1])

            def thumb(im):
                return list(im.crop(view_only).convert("L").resize((40, 21)).tobytes())

            def delta(a, b):
                return sum(abs(x - y) for x, y in zip(thumb(a), thumb(b))) / (40 * 21)

            vm.key_down("right"); time.sleep(0.5)
            turn_a = vm.image(); time.sleep(0.4); turn_b = vm.image()
            vm.key_up("right"); time.sleep(1.2)
            still_a = vm.image(); time.sleep(0.4); still_b = vm.image()
            turning, resting = delta(turn_a, turn_b), delta(still_a, still_b)
            check("holding a turn key sweeps the view", turning > 4.0, "mean change %.1f" % turning)
            check("releasing it stops the turn", resting < turning / 3,
                  "mean change %.1f while turning, %.1f after release" % (turning, resting))

            # The mouse turns too (relative motion, read by the game itself
            # while it has the screen).
            m0 = vm.image()
            vm.mouse_rel(240, 0); time.sleep(0.7)
            vm._mx = None                           # the guest pointer is wherever Doom left it
            m1 = vm.image()
            check("moving the mouse turns the view", delta(m0, m1) > 2.0,
                  "mean change %.1f" % delta(m0, m1))
            still_b = m1

            vm.key_down("up"); time.sleep(1.0); vm.key_up("up"); time.sleep(0.6)
            moved = vm.image()
            check("holding Up moves the player", delta(still_b, moved) > 2.0,
                  "mean change %.1f" % delta(still_b, moved))

            # Fire: the ammo counter (left end of the status bar) changes.
            ammo = (HUD[0], HUD[1], HUD[0] + 100, HUD[3])
            a0 = vm.image()
            for _ in range(3):
                vm.key("ctrl", hold=120); time.sleep(0.5)
            time.sleep(0.5)
            a1 = vm.image(); keep(a1, "03-fired")
            check("firing uses ammunition", differs(a0, a1, ammo))

            # Save into the first slot under a typed name with letters.
            vm.key("esc"); time.sleep(0.6)
            for _ in range(3):
                vm.key("down"); time.sleep(0.3)      # New, Options, Load, [Save]
            vm.key("ret"); time.sleep(0.6)           # Save Game -> slot list
            vm.key("ret"); time.sleep(0.5)           # pick slot 1, start typing
            vm.type("qasave"); time.sleep(0.3)
            keep(vm.image(), "04-save-name")
            vm.key("ret"); time.sleep(2.5)           # write it

            quit_doom(vm, mark)
            time.sleep(1.0)
            after = d.shot(); keep(after, "05-desktop-after")
            pa, pb = after.load(), base.load()
            bad = sum(1 for y in range(768) for x in range(1024)
                      if pa[x, y] != pb[x, y] and not (x >= clock[0] and y >= clock[1]))
            check("desktop is intact after Doom quits", bad == 0, "%d pixels differ" % bad)

            # Second run: load the game saved in the first.
            mark = start_doom(vm, d)
            vm.key("esc"); time.sleep(0.6)
            for _ in range(2):
                vm.key("down"); time.sleep(0.3)      # New, Options, [Load]
            vm.key("ret"); time.sleep(0.6)
            keep(vm.image(), "06-load-menu")
            vm.key("ret"); time.sleep(5.0)           # slot 1
            loaded = vm.image(); keep(loaded, "07-loaded")
            check("second run: the saved game loads into play", colours(loaded, HUD) > 30,
                  colours(loaded, HUD))
            quit_doom(vm, mark)

            log = vm.serial()
            trouble = [l for l in log.splitlines()
                       if "kernel exception" in l or "stack overflow" in l or "app: fault" in l
                       or "OUT OF MEMORY" in l or "bytes of kernel stack" in l]
            check("no kernel exception, fault, stack warning or OOM", not trouble, "; ".join(trouble[:3]))

        new = sorted(set(n for n, _, _ in fs_list(img)) - before)
        saves = [n for n in new if n.lower().endswith(".dsg")]
        check("the save file is named for its slot", "doomsav0.dsg" in [n.lower() for n in new],
              "new files: %s" % new)
        if saves:
            data = fs_read(img, saves[0])
            check("the save file carries the typed name", data[:6].lower() == b"qasave", repr(data[:24]))
    finally:
        os.remove(img)

    failed = [c for c in checks if not c[1]]
    print("doom_test: %d/%d checks passed" % (len(checks) - len(failed), len(checks)))
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except QaError as e:
        print("FAIL (harness): " + str(e).splitlines()[0])
        sys.exit(1)
