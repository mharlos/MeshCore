#!/usr/bin/env python3
"""Set up a farm pager (env t1000e_pager) over USB, using the companion radio protocol.

  python pager.py COM15 info
  python pager.py COM15 name Cody
  python pager.py COM15 chans
  python pager.py COM15 addchan farm <32 hex key>     # the key from the deck's `key farm`
  python pager.py COM15 delchan <idx>
  python pager.py COM15 pin 123456                   # BLE pairing PIN (takes effect on reboot)
  python pager.py COM15 time                         # set its clock from this PC
  python pager.py COM15 advert                       # flood an advert now
  python pager.py COM15 reboot
  python pager.py COM15 wipe                         # factory reset: new identity, no channels

Several commands can go in one run, separated by "+":  name Cody + time + chans
"""
import struct
import sys
import time

import serial

CMD_APP_START, CMD_SET_DEVICE_TIME, CMD_SEND_SELF_ADVERT, CMD_SET_ADVERT_NAME = 1, 6, 7, 8
CMD_REBOOT, CMD_GET_BATT, CMD_GET_CHANNEL, CMD_SET_CHANNEL = 19, 20, 31, 32
CMD_SET_DEVICE_PIN, CMD_FACTORY_RESET, CMD_DEVICE_QUERY = 37, 51, 22
RESP_OK, RESP_ERR, RESP_SELF_INFO, RESP_BATT, RESP_CHANNEL, RESP_DEVICE_INFO = 0, 1, 5, 12, 18, 13


def open_port(port):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = True   # nRF52 USB serial only talks to a host that raises DTR (and doesn't reset on it)
    s.rts = True
    s.open()
    time.sleep(0.3)
    s.reset_input_buffer()
    return s


def send(s, payload):
    s.write(b"<" + struct.pack("<H", len(payload)) + payload)


def recv(s, want=None, timeout=3.0):
    """next response frame (optionally one starting with a code in `want`), skipping pushes"""
    end = time.time() + timeout
    buf = b""
    while time.time() < end:
        buf += s.read(512)
        while True:
            i = buf.find(b">")
            if i < 0 or len(buf) < i + 3:
                break
            n = struct.unpack("<H", buf[i + 1:i + 3])[0]
            if len(buf) < i + 3 + n:
                break
            frame, buf = buf[i + 3:i + 3 + n], buf[i + 3 + n:]
            if frame and (want is None or frame[0] in want) and frame[0] < 0x80:
                return frame
    return None


def ok(s, payload, what):
    send(s, payload)
    r = recv(s, (RESP_OK, RESP_ERR))
    print(f"{what}: {'OK' if r and r[0] == RESP_OK else 'FAILED'}")
    return r is not None and r[0] == RESP_OK


def info(s):
    send(s, bytes([CMD_APP_START]) + b"\x03" + b"\0" * 6 + b"pager.py")
    r = recv(s, (RESP_SELF_INFO,))
    if not r:
        print("no reply - is this a MeshCore companion?")
        return
    freq, bw = struct.unpack("<II", r[48:56])
    print(f"name   {r[58:].decode(errors='replace')}")
    print(f"key    {r[4:36].hex()[:16]}...")
    print(f"radio  {freq / 1000:.3f} MHz  BW {bw / 1000:g}  SF{r[56]}  CR{r[57]}  {r[2]} dBm")
    send(s, bytes([CMD_GET_BATT]))
    b = recv(s, (RESP_BATT,))
    if b:
        print(f"batt   {struct.unpack('<H', b[1:3])[0]} mV")
    send(s, bytes([CMD_DEVICE_QUERY, 3]))
    d = recv(s, (RESP_DEVICE_INFO,))
    if d:
        pin = struct.unpack("<I", d[4:8])[0]
        print(f"pin    {pin:06d}" if pin else "pin    build default")


def chans(s):
    for i in range(8):
        send(s, bytes([CMD_GET_CHANNEL, i]))
        r = recv(s, (RESP_CHANNEL, RESP_ERR), 1.5)
        if not r or r[0] != RESP_CHANNEL:
            break
        name = r[2:34].split(b"\0")[0].decode(errors="replace")
        if name:
            print(f"  [{i}] {name}")   # keys stay off the screen


def set_chan(s, idx, name, key):
    payload = bytes([CMD_SET_CHANNEL, idx]) + name.encode()[:31].ljust(32, b"\0") + key
    return ok(s, payload, f"channel [{idx}] {name or '(deleted)'}")


def free_slot(s):
    for i in range(8):
        send(s, bytes([CMD_GET_CHANNEL, i]))
        r = recv(s, (RESP_CHANNEL, RESP_ERR), 1.5)
        if not r or r[0] != RESP_CHANNEL or not r[2:34].split(b"\0")[0]:
            return i
    return None


def run(s, args):
    cmd = args[0].lower()
    if cmd == "info":
        info(s)
    elif cmd == "name":
        ok(s, bytes([CMD_SET_ADVERT_NAME]) + " ".join(args[1:]).encode(), "name")
    elif cmd == "chans":
        chans(s)
    elif cmd == "addchan":
        name, key = args[1], bytes.fromhex(args[2])
        assert len(key) == 16, "key must be 32 hex characters"
        idx = free_slot(s)
        if idx is None:
            print("no free channel slot")
        else:
            set_chan(s, idx, name, key)
    elif cmd == "delchan":
        set_chan(s, int(args[1]), "", b"\0" * 16)
    elif cmd == "pin":
        ok(s, bytes([CMD_SET_DEVICE_PIN]) + struct.pack("<I", int(args[1])), "BLE PIN (reboot to apply)")
    elif cmd == "time":
        ok(s, bytes([CMD_SET_DEVICE_TIME]) + struct.pack("<I", int(time.time())), "clock")
    elif cmd == "advert":
        ok(s, bytes([CMD_SEND_SELF_ADVERT, 1]), "flood advert")
    elif cmd == "reboot":
        send(s, bytes([CMD_REBOOT]) + b"reboot")
        print("rebooting")
    elif cmd == "wipe":
        send(s, bytes([CMD_FACTORY_RESET]) + b"reset")
        try:
            recv(s, (RESP_OK, RESP_ERR))
        except serial.SerialException:
            pass   # it drops off USB as it reboots
        print("factory reset: wiped, rebooting with a new identity")
        raise SystemExit
    else:
        print(__doc__)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return
    s = open_port(sys.argv[1])
    try:
        send(s, bytes([CMD_APP_START]) + b"\x03" + b"\0" * 6 + b"pager.py")   # say hello first
        recv(s, (RESP_SELF_INFO,))
        group = []
        for a in sys.argv[2:] + ["+"]:
            if a == "+":
                if group:
                    run(s, group)
                group = []
            else:
                group.append(a)
    finally:
        s.close()


if __name__ == "__main__":
    main()
