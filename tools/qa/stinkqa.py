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
        # STINKQA_KVM=1 runs every suite on the host's CPU through KVM
        # instead of the emulated one: real instruction timing, and a
        # different interleaving of everything that depends on it.
        kvm = os.environ.get("STINKQA_KVM")
        if kvm:
            cpu = "host"
        args = ["qemu-system-i386", "-cpu", cpu,
                "-drive", "format=raw,file=%s" % image,
                "-display", display,
                "-serial", "file:" + self.serial_path,
                "-qmp", "unix:%s,server,nowait" % self.qmp_path,
                "-no-reboot"]
        if kvm:
            args.append("-enable-kvm")
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
