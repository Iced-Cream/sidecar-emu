#!/usr/bin/env python3
"""freeport.py: host ports for one emulator run, so several runs can share a host.

  freeport.py PREFERRED [PREFERRED ...]   prints one port per argument, space-separated
  freeport.py --wait-bound PORT [SECS]    exits 0 once something listens on 127.0.0.1:PORT (default 10 s), else 1

Each port is the preferred one when nothing on 127.0.0.1 holds it, otherwise one the kernel hands out as free; the
ports printed are all different. A port is only checked, not held, so a run picks its ports and starts QEMU under
the host-wide lock LOCK (flock), and keeps the lock until QEMU listens on them (--wait-bound); two runs starting in
the same instant then cannot both pick the same port. twobox-gate.py imports this; teensy-gate.sh runs it.
"""
import fcntl, os, socket, sys, tempfile, time

LOCK = os.path.join(tempfile.gettempdir(), "sidecar-emu-ports.lock")


def free(port):
    s = socket.socket()
    try:
        s.bind(("127.0.0.1", port)); return True
    except OSError:
        return False
    finally:
        s.close()


def pick(*preferred):
    out = []
    for p in preferred:
        if p in out or not free(p):
            while True:
                s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close()
                if p not in out:
                    break
        out.append(p)
    return out


def wait_bound(port, secs=10.0, alive=lambda: True):
    end = time.time() + secs
    while time.time() < end and alive():
        if not free(port):
            return True
        time.sleep(0.1)
    return False


class PortLock:
    """with PortLock(): pick ports, start QEMU, wait_bound() its ports"""
    def __enter__(self):
        self.f = open(LOCK, "a"); fcntl.flock(self.f, fcntl.LOCK_EX); return self

    def __exit__(self, *exc):
        fcntl.flock(self.f, fcntl.LOCK_UN); self.f.close()


if __name__ == "__main__":
    if sys.argv[1:2] == ["--wait-bound"]:
        sys.exit(0 if wait_bound(int(sys.argv[2]), float(sys.argv[3]) if len(sys.argv) > 3 else 10.0) else 1)
    print(" ".join(str(p) for p in pick(*(int(a) for a in sys.argv[1:]))))
