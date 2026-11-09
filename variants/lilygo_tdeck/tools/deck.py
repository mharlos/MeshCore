#!/usr/bin/env python3
"""Drive a T-Deck running the cyberdeck UI (built with CD_SERIAL_BRIDGE) over USB serial.

  deck.py COM13 shot out.png             save a screenshot
  deck.py COM13 keys "hello" enter       type text, then press keys
  deck.py COM13 log 10                   print serial output for 10 seconds

Key names: up down left right click hold enter esc bs shot:<file> wait:<secs>.
Anything else is typed literally.
"""
import sys, time
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
    elif cmd == "log":
        end = time.time() + float(args[0] if args else 10)
        while time.time() < end:
            d = s.read(4096)
            if d:
                sys.stdout.write(d.decode("utf-8", "replace"))
                sys.stdout.flush()


if __name__ == "__main__":
    main()
