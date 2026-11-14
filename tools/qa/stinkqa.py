#!/usr/bin/env python3
"""QEMU driver for StinkOS QA: boot an image, inject keyboard + mouse input
over QMP, read the serial log and take screenshots.

Development-only tooling: nothing here runs inside StinkOS. Every VM gets
its own temp directory (QMP socket, serial log), so several can run at once
and no test shares state through fixed /tmp paths.

By default the guest disk is opened with -snapshot, so os.bin is never
modified. Persistence tests pass snapshot=False together with a private
copy of the image (see Vm.copy_image).

Typical use:

    from stinkqa import Vm
    with Vm() as vm:
        vm.wait_serial("rs-desktop: start")
        vm.mouse_to(100, 120); vm.click()
        vm.screenshot("out.png")
"""
import json
import os
import shutil
import socket
import subprocess
import tempfile
import time

SCREEN_W = 1024
SCREEN_H = 768

# Characters that need a qcode other than themselves (US layout).
_QCODE = {
    " ": "spc", "\n": "ret", "\t": "tab", ".": "dot", ",": "comma",
    "-": "minus", "=": "equal", "/": "slash", ";": "semicolon",
    "'": "apostrophe", "[": "bracket_left", "]": "bracket_right",
    "\\": "backslash", "`": "grave_accent",
}
_SHIFTED = {
    "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7",
    "*": "8", "(": "9", ")": "0", "_": "minus", "+": "equal", ":": "semicolon",
    '"': "apostrophe", "<": "comma", ">": "dot", "?": "slash",
    "{": "bracket_left", "}": "bracket_right", "|": "backslash",
    "~": "grave_accent",
}


class QaError(RuntimeError):
    pass


class Vm:
    def __init__(self, image="os.bin", snapshot=True, mem=None, cpu="Penryn",
                 net=False, audio=False, extra=None, display="none",
                 hostfwd=None):
        self.image_path = image
        self.dir = tempfile.mkdtemp(prefix="stinkqa-")
        self.serial_path = os.path.join(self.dir, "serial.log")
        self.qmp_path = os.path.join(self.dir, "qmp.sock")
        self._mx = None            # last known guest cursor position
        self._my = None
        args = ["qemu-system-i386", "-cpu", cpu,
                "-drive", "format=raw,file=%s" % image,
                "-display", display,
                "-serial", "file:" + self.serial_path,
                "-qmp", "unix:%s,server,nowait" % self.qmp_path,
                "-no-reboot"]
        if snapshot:
            args.append("-snapshot")
        if mem:
            args += ["-m", str(mem)]
        if net:
            # hostfwd: [(host_port, guest_port), ...] TCP forwards into the guest
            netdev = "user,id=net0"
            for hp, gp in (hostfwd or []):
                netdev += ",hostfwd=tcp:127.0.0.1:%d-:%d" % (hp, gp)
            args += ["-netdev", netdev, "-device", "e1000,netdev=net0"]
        if audio:
            args += ["-audiodev", "none,id=snd0", "-device", "sb16,audiodev=snd0"]
        if extra:
            args += list(extra)
        self.args = args
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.PIPE)
        self.sock = None
        self._buf = b""
        self._connect()

    # ---- lifecycle -------------------------------------------------------

    def _connect(self):
        deadline = time.time() + 10
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise QaError("qemu exited: " +
                              self.proc.stderr.read().decode(errors="replace"))
            try:
                s = socket.socket(socket.AF_UNIX)
                s.connect(self.qmp_path)
                self.sock = s
                break
            except OSError:
                time.sleep(0.05)
        else:
            raise QaError("could not connect to QMP")
        self._read_msg()                        # greeting
        self.qmp("qmp_capabilities")

    def close(self):
        if self.proc.poll() is None:
            try:
                self.qmp("quit")
            except Exception:
                pass
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        if self.sock:
            self.sock.close()
            self.sock = None
        shutil.rmtree(self.dir, ignore_errors=True)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    def alive(self):
        return self.proc.poll() is None

    def wait_exit(self, timeout=10):
        """Wait for QEMU to exit by itself (guest shutdown). True if it did."""
        try:
            self.proc.wait(timeout=timeout)
            return True
        except subprocess.TimeoutExpired:
            return False

    @staticmethod
    def copy_image(src="os.bin", dst=None):
        """Private writable copy of a disk image, for persistence tests."""
        if dst is None:
            fd, dst = tempfile.mkstemp(prefix="stinkqa-", suffix=".bin")
            os.close(fd)
        # --sparse keeps the ~100 MiB mostly-empty image cheap to copy.
        subprocess.check_call(["cp", "--sparse=always", src, dst])
        return dst

    # ---- QMP -------------------------------------------------------------

    def _read_msg(self, timeout=10):
        self.sock.settimeout(timeout)
        while b"\n" not in self._buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise QaError("QMP connection closed")
            self._buf += chunk
        line, self._buf = self._buf.split(b"\n", 1)
        return json.loads(line)

    def qmp(self, command, **arguments):
        msg = {"execute": command}
        if arguments:
            msg["arguments"] = arguments
        self.sock.sendall(json.dumps(msg).encode() + b"\n")
        while True:
            reply = self._read_msg()
            if "event" in reply:
                continue                        # async event, not our answer
            if "error" in reply:
                raise QaError("%s: %s" % (command, reply["error"]))
            return reply.get("return")

    def hmp(self, line):
        return self.qmp("human-monitor-command", **{"command-line": line})

    # ---- serial ----------------------------------------------------------

    def serial(self):
        try:
            with open(self.serial_path, "rb") as f:
                return f.read().decode(errors="replace")
        except OSError:
            return ""

    def wait_serial(self, text, timeout=15, start=0):
        """Block until `text` shows up in the serial log (searching from
        offset `start`). Returns the offset just past the match."""
        deadline = time.time() + timeout
        while True:
            log = self.serial()
            i = log.find(text, start)
            if i >= 0:
                return i + len(text)
            if not self.alive():
                raise QaError("qemu died waiting for %r\n%s" % (text, log[-2000:]))
            if time.time() > deadline:
                raise QaError("timeout waiting for %r\n--- serial tail ---\n%s"
                              % (text, log[-2000:]))
            time.sleep(0.03)

    def mark(self):
        """Current end of the serial log, to pass as wait_serial(start=...)."""
        return len(self.serial())

    def serial_count(self, text):
        return self.serial().count(text)

    # ---- keyboard --------------------------------------------------------

    def key(self, *qcodes, hold=40):
        """Press a chord (e.g. key('ctrl', 's')) and release it."""
        keys = [{"type": "qcode", "data": q} for q in qcodes]
        self.qmp("send-key", keys=keys, **{"hold-time": hold})
        time.sleep(hold / 1000.0 + 0.04)

    def key_down(self, qcode):
        self._input([{"type": "key", "data": {
            "down": True, "key": {"type": "qcode", "data": qcode}}}])

    def key_up(self, qcode):
        self._input([{"type": "key", "data": {
            "down": False, "key": {"type": "qcode", "data": qcode}}}])

    def type(self, text, hold=30):
        for ch in text:
            if ch in _SHIFTED:
                self.key("shift", _SHIFTED[ch], hold=hold)
            elif ch.isupper():
                self.key("shift", ch.lower(), hold=hold)
            else:
                self.key(_QCODE.get(ch, ch), hold=hold)

    # ---- mouse -----------------------------------------------------------

    def _input(self, events):
        self.qmp("input-send-event", events=events)

    def mouse_rel(self, dx, dy):
        """Relative motion, split so each PS/2 packet stays well in range."""
        step = 100
        while dx or dy:
            sx = max(-step, min(step, dx))
            sy = max(-step, min(step, dy))
            ev = []
            if sx:
                ev.append({"type": "rel", "data": {"axis": "x", "value": sx}})
            if sy:
                ev.append({"type": "rel", "data": {"axis": "y", "value": sy}})
            self._input(ev)
            dx -= sx
            dy -= sy
            time.sleep(0.012)
        if self._mx is not None:
            pass

    def mouse_home(self):
        """Pin the guest cursor to (0, 0): the driver clamps at the edges."""
        self.mouse_rel(-(SCREEN_W + 200), -(SCREEN_H + 200))
        self._mx, self._my = 0, 0
        time.sleep(0.05)

    def mouse_to(self, x, y):
        if self._mx is None:
            self.mouse_home()
        self.mouse_rel(x - self._mx, y - self._my)
        self._mx, self._my = x, y
        time.sleep(0.05)

    def button(self, down, which="left"):
        self._input([{"type": "btn", "data": {"down": down, "button": which}}])
        time.sleep(0.05)

    def click(self, x=None, y=None, which="left", hold=0.08):
        if x is not None:
            self.mouse_to(x, y)
        self.button(True, which)
        time.sleep(hold)
        self.button(False, which)
        time.sleep(0.05)

    def drag(self, x0, y0, x1, y1, steps=8, which="left"):
        self.mouse_to(x0, y0)
        self.button(True, which)
        time.sleep(0.08)
        for i in range(1, steps + 1):
            self.mouse_to(x0 + (x1 - x0) * i // steps,
                          y0 + (y1 - y0) * i // steps)
            time.sleep(0.04)
        time.sleep(0.08)
        self.button(False, which)
        time.sleep(0.08)

    # ---- screen ----------------------------------------------------------

    def screenshot(self, path):
        """Dump the guest display to a PNG file and return its path."""
        path = os.path.abspath(path)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.qmp("screendump", filename=path, format="png")
        return path

    def image(self):
        """Current guest display as a PIL RGB image."""
        from PIL import Image
        tmp = os.path.join(self.dir, "shot.png")
        self.screenshot(tmp)
        with Image.open(tmp) as im:
            return im.convert("RGB").copy()

    def wait_stable(self, timeout=5, settle=0.15):
        """Wait until two consecutive screenshots are identical; returns the
        stable image. Raises if the screen never settles (animation)."""
        deadline = time.time() + timeout
        prev = self.image()
        while time.time() < deadline:
            time.sleep(settle)
            cur = self.image()
            if cur.tobytes() == prev.tobytes():
                return cur
            prev = cur
        raise QaError("screen did not settle within %.1fs" % timeout)


def diff_bbox(a, b):
    """Bounding box (x0, y0, x1, y1) of the pixels that differ, or None."""
    from PIL import ImageChops
    return ImageChops.difference(a, b).getbbox()


def count_color(img, rgb, box=None):
    """Number of pixels exactly equal to `rgb` inside `box`."""
    if box:
        img = img.crop(box)
    target = tuple(rgb)
    return sum(1 for p in img.getdata() if p == target)


# ---- StinkFS image editing (host side) -----------------------------------
#
# Mirrors the on-disk layout in kernel/fs/fs.c and tools/make-stinkfs.py:
#   directory at FS_DIR_LBA, 4 sectors:
#     u32 magic 'STNK', u32 count, u32 next_free, 80 x {char name[16]; u32 lba; u32 size}
# Only ever used on private copies of the image (Vm.copy_image).

FS_DIR_LBA = 512
FS_DATA_END = 200516
_FS_MAGIC = 0x4B4E5453
_FS_MAX_FILES = 80
_SECTOR = 512


def _fs_read_dir(f):
    import struct
    f.seek(FS_DIR_LBA * _SECTOR)
    raw = f.read(4 * _SECTOR)
    magic, count, next_free = struct.unpack_from("<III", raw, 0)
    if magic != _FS_MAGIC:
        raise QaError("image has no StinkFS directory")
    entries = []
    for i in range(count):
        name, lba, size = struct.unpack_from("<16sII", raw, 12 + i * 24)
        entries.append([name.split(b"\0")[0].decode(), lba, size])
    return entries, next_free


def _fs_write_dir(f, entries, next_free):
    import struct
    raw = struct.pack("<III", _FS_MAGIC, len(entries), next_free)
    for name, lba, size in entries:
        raw += struct.pack("<16sII", name.encode()[:15], lba, size)
    raw += b"\0" * (4 * _SECTOR - len(raw))
    f.seek(FS_DIR_LBA * _SECTOR)
    f.write(raw)


def fs_list(image):
    with open(image, "rb") as f:
        return _fs_read_dir(f)[0]


def fs_read(image, name):
    with open(image, "rb") as f:
        for n, lba, size in _fs_read_dir(f)[0]:
            if n.upper() == name.upper():
                f.seek(lba * _SECTOR)
                return f.read(size)
    return None


def fs_put(image, name, data):
    """Create or replace `name` in the image's StinkFS (appended at next_free)."""
    if isinstance(data, str):
        data = data.encode()
    with open(image, "r+b") as f:
        entries, next_free = _fs_read_dir(f)
        entries = [e for e in entries if e[0].upper() != name.upper()]
        if len(entries) >= _FS_MAX_FILES:
            raise QaError("StinkFS directory full")
        sectors = (len(data) + _SECTOR - 1) // _SECTOR
        if next_free + sectors > FS_DATA_END:
            raise QaError("StinkFS data region full")
        f.seek(next_free * _SECTOR)
        f.write(data + b"\0" * (sectors * _SECTOR - len(data)))
        entries.append([name, next_free, len(data)])
        _fs_write_dir(f, entries, next_free + sectors)


def menu_image(src="os.bin"):
    """Private copy of the image that boots into the kernel's text menu
    (no AUTORUN, so no desktop). Deleted when the calling script exits.
    For the scripts that drive the menu and the legacy full-screen apps."""
    import atexit
    img = Vm.copy_image(src)
    fs_remove(img, "AUTORUN")
    atexit.register(lambda: os.path.exists(img) and os.remove(img))
    return img


def fs_remove(image, name):
    with open(image, "r+b") as f:
        entries, next_free = _fs_read_dir(f)
        kept = [e for e in entries if e[0].upper() != name.upper()]
        _fs_write_dir(f, kept, next_free)
        return len(kept) != len(entries)


# ---- reading text off the screen -------------------------------------------
#
# The UI font is a fixed 8x16 bitmap (lib/libstink_font.c), so text in a
# screenshot can be read back exactly by matching glyphs: no fuzzy OCR.

_GLYPHS = None


def _load_font():
    global _GLYPHS
    if _GLYPHS is None:
        import re
        here = os.path.dirname(os.path.abspath(__file__))
        src = open(os.path.join(here, "..", "..", "lib", "libstink_font.c")).read()
        table = src[src.index("FONT[95][16]"):]
        _GLYPHS = {}
        for i, m in enumerate(re.finditer(r"\{((?:\s*0x[0-9a-fA-F]{2}\s*,?){16})\}", table)):
            if i >= 95:
                break
            rows = tuple(int(v, 16) for v in re.findall(r"0x[0-9a-fA-F]{2}", m.group(1)))
            if any(rows):
                _GLYPHS[rows] = chr(32 + i)
    return _GLYPHS


def read_text(img, box, colors):
    """Lines of text drawn in any of `colors` (RGB tuples) inside `box`
    (x0, y0, x1, y1). Returns a list of (y, string), top to bottom; runs of
    more than one empty cell become a single space."""
    glyphs = _load_font()
    x0, y0, x1, y1 = box
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(img.size[0], x1), min(img.size[1], y1)
    px = img.load()
    colors = set(tuple(c) for c in colors)
    ink = [[px[x, y] in colors for x in range(x0, x1)] for y in range(y0, y1)]
    w, h = x1 - x0, y1 - y0
    def parse(y):
        found, x, last_end = [], 0, None
        while x + 8 <= w:
            rows = tuple(sum((0x80 >> c) for c in range(8) if ink[y + r][x + c]) for r in range(16))
            ch = glyphs.get(rows)
            if ch:
                if last_end is not None and x - last_end >= 8:
                    found.append(" ")
                found.append(ch)
                last_end = x + 8
                x += 8
            else:
                x += 1
        return "".join(found)

    lines = []
    y = 0
    while y + 16 <= h:
        best = parse(y)
        if not best.strip():
            y += 1
            continue
        # A stray shape can match one glyph a few rows above a real line of
        # text: look through the next rows and keep the fullest reading.
        best_y = y
        for dy in range(1, 16):
            if y + dy + 16 > h:
                break
            cand = parse(y + dy)
            if len(cand.replace(" ", "")) > len(best.replace(" ", "")):
                best, best_y = cand, y + dy
        lines.append((y0 + best_y, best))
        y = best_y + 16
    return lines


def screen_text(img, box, colors):
    """All text in the box as one string, lines joined with newlines."""
    return "\n".join(s for _, s in read_text(img, box, colors))


def find_text(img, box, colors, needle):
    """Centre (x, y) of the first occurrence of `needle` drawn in `colors`
    inside `box`, or None. Exact glyph match, any pixel offset."""
    glyphs = _load_font()
    blank = (0,) * 16
    want = [blank if c == " " else next((rows for rows, ch in glyphs.items() if ch == c), None)
            for c in needle]
    if not needle.strip() or any(w is None for w in want):
        return None
    x0, y0, x1, y1 = box
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(img.size[0], x1), min(img.size[1], y1)
    px = img.load()
    colors = set(tuple(c) for c in colors)
    ink = [[px[x, y] in colors for x in range(x0, x1)] for y in range(y0, y1)]
    w, h = x1 - x0, y1 - y0

    def cell(x, y):
        return tuple(sum((0x80 >> c) for c in range(8) if ink[y + r][x + c]) for r in range(16))

    n = len(needle)
    for y in range(0, h - 15):
        for x in range(0, w - 8 * n + 1):
            if all(cell(x + 8 * i, y) == want[i] for i in range(n)):
                return (x0 + x + 4 * n, y0 + y + 8)
    return None


# Colours text is drawn in (apps/rust/libui palette).
UI_TEXT = [(0xe6, 0xed, 0xf3), (0x8b, 0x94, 0x9e), (0x57, 0xf2, 0x87),
           (0xfe, 0xe7, 0x5c), (0xf4, 0x70, 0x67)]


class Desktop:
    """Helpers for scripts that drive the graphical desktop: launch a
    program by name, follow its window through the kernel's serial trace."""

    PARK = (1012, 700)

    def __init__(self, vm):
        import re
        self.vm = vm
        self._re = re
        vm.wait_serial("desktop: ready", timeout=60)
        self.tiles = {}
        for m in re.finditer(r"desktop: tile (\S+) (\d+) (\d+) (\d+) (\d+)", vm.serial()):
            x, y, w, h = map(int, m.groups()[1:])
            self.tiles[m.group(1)] = (x + w // 2, y + h // 2)

    def launch(self, name, timeout=20):
        """Click a tile and return the pid of the window that appears."""
        mark = self.vm.mark()
        self.vm.click(*self.tiles[name])
        end = self.vm.wait_serial("win: show pid=", timeout=timeout, start=mark)
        return int(self._re.match(r"(\d+)", self.vm.serial()[end:]).group(1))

    def geom(self, pid):
        """Latest client rectangle (x, y, w, h) of pid's window."""
        last = None
        pat = r"win: (?:show|moved|resize) pid=%d x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)" % pid
        for m in self._re.finditer(pat, self.vm.serial()):
            last = tuple(map(int, m.groups()))
        return last

    def closed(self, pid, start=0):
        return ("win: destroy pid=%d " % pid) in self.vm.serial()[start:]

    def wait_closed(self, pid, start=0, timeout=6):
        self.vm.wait_serial("win: destroy pid=%d " % pid, timeout=timeout, start=start)

    def shot(self, settle=True):
        """Screenshot with the pointer parked out of the way."""
        self.vm.mouse_to(*self.PARK)
        if settle:
            try:
                return self.vm.wait_stable(timeout=4)
            except QaError:
                pass                             # something is animating: take it as is
        return self.vm.image()

    def text(self, pid, img=None):
        """All text in pid's client area."""
        x, y, w, h = self.geom(pid)
        img = img or self.shot()
        return screen_text(img, (x, y, x + w, y + h), UI_TEXT)

    def click_text(self, pid, needle, img=None):
        """Click the first occurrence of `needle` in pid's window."""
        x, y, w, h = self.geom(pid)
        img = img or self.shot()
        at = find_text(img, (x, y, x + w, y + h), UI_TEXT, needle)
        if not at:
            raise QaError("text %r not found in window of pid %d" % (needle, pid))
        self.vm.click(*at)
        return at
