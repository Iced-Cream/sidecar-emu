#!/usr/bin/env python3
"""The computer on the far side of usb-teensy (hw/usb/dev-teensy.c), for testing the Pi engine's Teensy relay
without a Teensy.

usb-teensy's chardev connects here (run-sidecar.sh TEENSY=127.0.0.1:7302, or TEENSY=unix:PATH with --port unix:PATH). When the relay shows a box ('V' 1) this
script does what a computer does with it: bus reset, the device, configuration and string descriptors (answered by the
stub from what the Pi uploaded), SET_CONFIGURATION, then vendor requests that the relay forwards to the Pi and the Pi
forwards to the box (Overbridge's unit ID, name, OS version, protocol versions: the Rytm's requests 0, 1, 2, 4), and
optionally SET_INTERFACE alt 3 on interfaces 1 and 2 and the computer's audio OUT blocks to 0x03. It prints each answer
and a line a second with what it got.

  python3 sidecar/tools/teensystub.py [--port 7302|unix:PATH] [--audio] [--flash-build NAME] [--button-at S] [--replug-at S]
                                      [--exit-after S] [--exit-after-shown S] [--json FILE]

--flash-build sets the build the next flash comes back with ('B'); --button-at presses the program button S seconds
after the start (the Teensy enters HalfKay: the Pi's recovery path), --replug-at unplugs the Teensy and plugs it back
2 s later. --exit-after-shown ends S seconds after the relay first shows a box (boot times vary). --json writes the counters and every forwarded answer at the end (for gate scripts).

Frames both ways: [kind u8][length u16 LE][payload] (dev-teensy.c's header comment).
"""
import argparse
import json
import os
import socket
import struct
import sys
import threading
import time

OB_BLOCK = 368


class Computer:
    def __init__(self, conn, args):
        self.c = conn
        self.a = args
        self.lock = threading.Lock()
        self.answer = None
        self.answer_ev = threading.Event()
        self.shown = threading.Event()
        self.st = {"shown": 0, "unshown": 0, "in_packets": {}, "in_bytes": 0, "halts": [], "lines": [],
                   "answers": [], "enumerations": 0, "enum_ok": 0}
        self.cfg = b""
        self.dev = b""
        self.stop = False

    # -- framing
    def send(self, kind, payload=b""):
        with self.lock:
            self.c.sendall(kind.encode() + struct.pack("<H", len(payload)) + payload)
        self.st["sent_" + kind] = self.st.get("sent_" + kind, 0) + 1

    def reader(self):
        buf = b""
        try:
            while not self.stop:
                d = self.c.recv(65536)
                if not d:
                    break
                buf += d
                while len(buf) >= 3:
                    n = struct.unpack("<H", buf[1:3])[0]
                    if len(buf) < 3 + n:
                        break
                    k, p, buf = chr(buf[0]), buf[3:3 + n], buf[3 + n:]
                    self.frame(k, p)
        except OSError:
            pass
        self.stop = True
        self.answer_ev.set()

    def frame(self, k, p):
        if k == "V":
            if p and p[0]:
                self.st["shown"] += 1
                print("relay shows a box", flush=True)
                self.shown.set()
            else:
                self.st["unshown"] += 1
                print("relay stopped showing the box", flush=True)
        elif k == "C":
            self.answer = (p[0], p[1:])
            self.answer_ev.set()
        elif k == "I":
            ep = "0x%02x" % p[0]
            self.st["in_packets"][ep] = self.st["in_packets"].get(ep, 0) + 1
            self.st["in_bytes"] += len(p) - 1
        elif k == "H":
            self.st["halts"].append((p[0], p[1]))
        elif k == "L":
            t = p.decode("utf-8", "replace")
            self.st["lines"].append(t)
            print("stub:", t, flush=True)

    # -- EP0
    def control(self, setup, data=b"", timeout=3.0):
        self.answer = None
        self.answer_ev.clear()
        self.send("S", bytes(setup) + data)
        if not self.answer_ev.wait(timeout) or self.answer is None:
            return None
        return self.answer

    def get_desc(self, typ, idx, length):
        r = self.control(struct.pack("<BBHHH", 0x80, 6, typ << 8 | idx, 0x409 if typ == 3 else 0, length))
        return r[1] if r and r[0] == 0 else None

    def enumerate(self):
        self.st["enumerations"] += 1
        self.send("R")
        time.sleep(0.05)
        self.dev = self.get_desc(1, 0, 18) or b""
        if len(self.dev) != 18:
            print("device descriptor: no answer", flush=True)
            return False
        vid, pid = struct.unpack("<HH", self.dev[8:12])
        head = self.get_desc(2, 0, 9) or b""
        total = struct.unpack("<H", head[2:4])[0] if len(head) >= 4 else 0
        self.cfg = self.get_desc(2, 0, total) or b""
        names = []
        for i in self.dev[14:17]:
            if i:
                s = self.get_desc(3, i, 255)
                names.append(s[2:].decode("utf-16-le", "replace") if s else "?")
        print("box %04x:%04x, %u B configuration, strings %s" % (vid, pid, len(self.cfg), names), flush=True)
        r = self.control(struct.pack("<BBHHH", 0x00, 9, 1, 0, 0))
        if not r or r[0]:
            print("SET_CONFIGURATION refused", flush=True)
            return False
        # the Rytm's Overbridge vendor requests, forwarded through the relay to the Pi and on to the box
        ok = True
        for req, n, what in ((0x00, 8, "unit id"), (0x01, 32, "name"), (0x02, 16, "OS"), (0x04, 4, "protocol")):
            t0 = time.time()
            r = self.control(struct.pack("<BBHHH", 0xC0, req, 0, 0, n), timeout=5)
            ms = (time.time() - t0) * 1000
            if r is None:
                print("vendor %02x (%s): no answer in 5 s" % (req, what), flush=True)
                self.st["answers"].append({"req": req, "status": "none"})
                ok = False
                continue
            st, d = r
            shown = d.rstrip(b"\0").decode("latin-1") if what in ("name", "OS") else d.hex()
            print("vendor %02x (%s): %s %r in %.0f ms" % (req, what, "STALL" if st else "ok", shown, ms), flush=True)
            self.st["answers"].append({"req": req, "status": "stall" if st else "ok", "data": d.hex(), "ms": round(ms)})
            ok = ok and st == 0
        if self.a.audio:
            for iface in (1, 2):
                r = self.control(struct.pack("<BBHHH", 0x01, 11, 3, iface, 0))
                print("SET_INTERFACE %u alt 3: %s" % (iface, "ok" if r and not r[0] else "refused"), flush=True)
        if ok:
            self.st["enum_ok"] += 1
        return ok

    def audio_out(self):
        t0 = time.time()
        sent = 0
        blk = bytearray(OB_BLOCK)
        try:
            while not self.stop:
                time.sleep(0.005)
                due = int((time.time() - t0) * 48000 / 7) - sent
                for _ in range(min(due, 64)):
                    struct.pack_into(">HH", blk, 0, 0x0700, (sent * 7) & 0xFFFF)
                    self.send("O", bytes([0x03]) + bytes(blk))
                    sent += 1
        except OSError:
            self.stop = True            # usb-teensy went away (QEMU ended)

    def run(self):
        threading.Thread(target=self.reader, daemon=True).start()
        if self.a.flash_build:
            self.send("B", self.a.flash_build.encode())
        t0 = time.time()
        done_button = done_replug = False
        last = 0
        audio = None
        first_shown = None
        while not self.stop:
            if self.shown.wait(0.2):
                self.shown.clear()
                if first_shown is None:
                    first_shown = time.time()
                time.sleep(0.1)
                self.enumerate()
                if self.a.audio and audio is None:
                    audio = threading.Thread(target=self.audio_out, daemon=True)
                    audio.start()
            now = time.time() - t0
            if self.a.button_at is not None and not done_button and now >= self.a.button_at:
                done_button = True
                print("program button", flush=True)
                self.send("P", b"\x02")
            if self.a.replug_at is not None and not done_replug and now >= self.a.replug_at:
                done_replug = True
                print("unplug", flush=True)
                self.send("P", b"\x00")
                time.sleep(2)
                print("plug", flush=True)
                self.send("P", b"\x01")
            if int(now) != last:
                last = int(now)
                print("t=%d shown=%d enum ok %d/%d in %s" % (now, self.st["shown"], self.st["enum_ok"],
                      self.st["enumerations"], self.st["in_packets"]), flush=True)
            if self.a.exit_after and now >= self.a.exit_after:
                break
            if self.a.exit_after_shown and first_shown and time.time() - first_shown >= self.a.exit_after_shown:
                break
        self.stop = True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="7302", help="TCP port on 127.0.0.1, or unix:PATH")
    ap.add_argument("--audio", action="store_true", help="SET_INTERFACE alt 3 and send audio OUT blocks")
    ap.add_argument("--flash-build", default="")
    ap.add_argument("--button-at", type=float)
    ap.add_argument("--replug-at", type=float)
    ap.add_argument("--exit-after", type=float, default=0)
    ap.add_argument("--exit-after-shown", type=float, default=0)
    ap.add_argument("--json")
    a = ap.parse_args()
    if a.port.startswith("unix:"):
        if os.path.exists(a.port[5:]):
            os.unlink(a.port[5:])
        srv = socket.socket(socket.AF_UNIX)
        srv.bind(a.port[5:])
    else:
        srv = socket.socket()
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", int(a.port)))
    srv.listen(1)
    print("waiting for usb-teensy on", a.port, flush=True)
    c, addr = srv.accept()
    print("usb-teensy connected", addr, flush=True)
    comp = Computer(c, a)
    try:
        comp.run()
    except KeyboardInterrupt:
        pass
    if a.json:
        json.dump(comp.st, open(a.json, "w"), indent=1)
    sys.exit(0 if comp.st["enum_ok"] else 1)


if __name__ == "__main__":
    main()
