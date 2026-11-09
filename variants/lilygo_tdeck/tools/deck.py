#!/usr/bin/env python3
r"""Drive a T-Deck running the cyberdeck UI (built with CD_SERIAL_BRIDGE) over USB serial.

  deck.py COM13 shot out.png             save a screenshot
  deck.py COM13 keys "hello" enter       type text, then press keys
  deck.py COM13 log 10                   print serial output for 10 seconds
  deck.py COM13 sd                       summarise the SD card
  deck.py COM13 pull /maps C:\maps        copy a folder tree off the SD card (rerun to resume)

Key names: up down left right click hold enter esc bs shot:<file> wait:<secs>.
Anything else is typed literally.
"""
import os, sys, time
import serial

W, H = 320, 240
NAMED = {"up": b"\x02", "down": b"\x03", "left": b"\x04", "right": b"\x05",
         "click": b"\x06", "hold": b"\x07", "enter": b"\r", "esc": b"\x1b", "bs": b"\x08",
         "fakepage": b"\x10", "sys": b"\x11"}


def shot(s, path):
    from PIL import Image
    s.reset_input_buffer()
    s.write(b"\x01")
    deadline = time.time() + 10
    buf = b""
    while b"#SHOT " not in buf:
        buf += s.read(256)
        if time.time() > deadline:
            raise SystemExit("no #SHOT header (is CD_SERIAL_BRIDGE built in?)")
    buf = buf[buf.index(b"#SHOT "):]
    buf = buf[buf.index(b"\n") + 1:]
    need = W * H * 2
    while len(buf) < need:
        d = s.read(need - len(buf))
        buf += d
        if time.time() > deadline:
            raise SystemExit(f"short screenshot: {len(buf)}/{need}")
    px = buf[:need]
    img = Image.new("RGB", (W, H))
    data = []
    for i in range(0, need, 2):
        v = px[i] | (px[i + 1] << 8)
        r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
        data.append(((r * 255) // 31, (g * 255) // 63, (b * 255) // 31))
    img.putdata(data)
    img = img.resize((W * 2, H * 2), Image.NEAREST)
    img.save(path)
    print("saved", path)


class Link:
    """Buffered reads over the serial port: lines and exact byte counts."""
    def __init__(self, s):
        self.s, self.buf = s, b""

    def _fill(self, deadline):
        d = self.s.read(65536 if self.s.in_waiting > 4096 else 4096)
        if not d and time.time() > deadline:
            raise TimeoutError("deck stopped answering")
        self.buf += d

    def line(self, timeout=20):
        deadline = time.time() + timeout
        while b"\n" not in self.buf:
            self._fill(deadline)
        ln, self.buf = self.buf.split(b"\n", 1)
        return ln.rstrip(b"\r").decode("utf-8", "replace")

    def exact(self, n, timeout=30):
        deadline = time.time() + timeout
        while len(self.buf) < n:
            self._fill(deadline)
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def header(self, prefixes, timeout=20):
        # skip anything else the firmware logs while we wait
        while True:
            ln = self.line(timeout)
            if ln.startswith(prefixes):
                return ln


def ls(link, path):
    link.s.write(b"\x14" + path.encode() + b"\n")
    h = link.header(("#LS", "#ERR"))
    if h.startswith("#ERR"):
        raise IOError(h)
    dirs, files = [], {}
    while True:
        ln = link.line()
        if ln.startswith("#END"):
            return dirs, files
        if ln.startswith("D "):
            dirs.append(ln[2:])
        elif ln.startswith("F "):
            name, size = ln[2:].rsplit(" ", 1)
            files[name] = int(size)


def fetch_dir(link, path, dest):
    import zlib
    link.s.write(b"\x13" + path.encode() + b"\n")
    h = link.header(("#DIR", "#ERR"))
    if h.startswith("#ERR"):
        raise IOError(h)
    os.makedirs(dest, exist_ok=True)
    n = nbytes = bad = 0
    while True:
        h = link.header(("#F ", "#END"))
        if h.startswith("#END"):
            return n, nbytes, bad
        name, size = h[3:].rsplit(" ", 1)
        size = int(size)
        data = link.exact(size)
        link.exact(1)   # the newline before the checksum
        c = link.header(("#C ",))
        if int(c[3:], 16) != zlib.crc32(data) & 0xFFFFFFFF:
            bad += 1
            continue
        with open(os.path.join(dest, name), "wb") as f:
            f.write(data)
        n += 1
        nbytes += size


def pull(s, remote, local):
    """Mirror a folder tree from the deck's SD card, a folder at a time; rerun to resume."""
    link = Link(s)
    s.timeout = 0.2
    s.reset_input_buffer()
    start, total_files, total_bytes, total_bad, skipped = time.time(), 0, 0, 0, 0

    # walk the tree first, so we know how much there is
    todo, stack = [], [remote]
    while stack:
        path = stack.pop()
        dirs, files = ls(link, path)
        if files:
            todo.append((path, files))
        stack.extend(sorted((path.rstrip("/") + "/" + d for d in dirs), reverse=True))
    want = sum(len(f) for _, f in todo)
    print(f"{len(todo)} folders, {want} files on the card", flush=True)

    for i, (path, files) in enumerate(todo, 1):
        dest = os.path.join(local, *path[len(remote):].strip("/").split("/"))
        have = all(os.path.exists(os.path.join(dest, n)) and os.path.getsize(os.path.join(dest, n)) == sz
                   for n, sz in files.items())
        if have:
            skipped += len(files)
            continue
        for attempt in range(3):
            try:
                n, nb, bad = fetch_dir(link, path, dest)
                break
            except TimeoutError:
                print(f"  timeout on {path}, retrying", flush=True)
                time.sleep(2)
                s.reset_input_buffer()
                link.buf = b""
        else:
            raise SystemExit(f"gave up on {path}")
        total_files += n
        total_bytes += nb
        total_bad += bad
        el = time.time() - start
        done = total_files + skipped
        rate = total_bytes / el / 1024 if el else 0
        eta = (want - done) / (total_files / el) / 60 if total_files and el else 0
        print(f"[{i}/{len(todo)}] {path}: {n} files{' ' + str(bad) + ' BAD' if bad else ''}  "
              f"total {done}/{want}  {rate:.0f}KB/s  ~{eta:.0f} min left", flush=True)

    print(f"done: copied {total_files} files ({total_bytes / 1048576:.1f}MB), already had {skipped}, "
          f"{total_bad} failed checksum, {(time.time() - start) / 60:.1f} min", flush=True)


def main():
    port, cmd, args = sys.argv[1], sys.argv[2], sys.argv[3:]
    s = serial.Serial(port, 115200, timeout=0.5)
    if cmd == "shot":
        shot(s, args[0] if args else "shot.png")
    elif cmd == "keys":
        # always start from the SYS console: typed text must never land in a chat by accident
        if "--chat" not in args:
            s.write(NAMED["sys"])
            s.flush()
            time.sleep(0.2)
        for a in args:
            if a == "--chat":
                continue
            if a in NAMED:
                s.write(NAMED[a])
            elif a.startswith("shot:"):
                time.sleep(0.3)
                shot(s, a[5:])
                continue
            elif a.startswith("wait:"):
                time.sleep(float(a[5:]))
                continue
            else:
                for ch in a.encode():
                    s.write(bytes([ch]))
                    time.sleep(0.02)
            s.flush()
            time.sleep(0.15)
    elif cmd == "sd":
        s.reset_input_buffer()
        s.write(b"\x12")
        end = time.time() + 120
        buf = b""
        while time.time() < end and b"#END" not in buf and b"#SD none" not in buf:
            buf += s.read(4096)
        text = buf.decode("utf-8", "replace")
        start = text.find("#SD")
        print(text[start:] if start >= 0 else text)
    elif cmd == "pull":
        pull(s, args[0] if args else "/maps", args[1] if len(args) > 1 else "maps")
    elif cmd == "log":
        end = time.time() + float(args[0] if args else 10)
        while time.time() < end:
            d = s.read(4096)
            if d:
                sys.stdout.write(d.decode("utf-8", "replace"))
                sys.stdout.flush()


if __name__ == "__main__":
    main()
