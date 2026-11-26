#!/usr/bin/env python3
"""twobox-gate.py: two boxes on one emulated Pi, in both attach orders, with a detach and reattach and an SD card.

A Sidecar kernel on raspi4b with usb-rytm and usb-a4 behind the high-speed board hub (run-sidecar.sh with LINK and
A4_LINK), a fresh FAT32 card on EMMC2 (CARD), and QMP (QMP). Each box's link is a stublink.py; a box attaches when its
link starts and detaches when it stops, so the script sets the attach order by starting the links in turn:

  rytm-a4   Rytm at boot, A4 once the web answers (+ATTACH_GAP s), A4 unplugged and plugged back
  a4-rytm   A4 at boot, Rytm once the web answers (+ATTACH_GAP s), Rytm unplugged and plugged back
  again     a boot on rytm-a4's card with both boxes, so what the first boot saved is read back

Several gates can run at once on one host, each with its own OUTDIR: the box links are unix sockets in a private
temporary folder, and the web port is HTTP_PORT if set, otherwise 18118 when free, otherwise a free port (picked per
step, logged in run.log and summary.json). Give parallel runs different CPUS. After each step each box's `attached` property is read over QMP (the emulator's own view of what is plugged),
/status is logged whenever it changes, and after QEMU stops the card's /, /.rytm, /.a4 and /.sidecar are listed.

The script judges the emulator only (PASS/FAIL lines): the boxes QEMU shows match each step, /status answers, the card
holds a readable FAT, no panic/assert in the UART log. What the engine does with the boxes (its slots, its folders)
is recorded in each step's summary.json and status.log for the kernel's own gate to judge; --expect-folders makes
the card's /.rytm and /.a4 part of the PASS.

  sidecar/tools/twobox-gate.py KERNEL OUTDIR [--words "..."] [--steps rytm-a4,a4-rytm,again] [--expect-folders]
Environment: QEMU, CPUS, HTTP_PORT (default: 18118 if free, else a free port) as run-sidecar.sh; ATTACH_GAP (default 20), HOLD (default 30:
seconds between the events after the second attach), WEB_WAIT (default 300). Kills only the processes it started.
"""
import argparse, fcntl, json, os, re, shutil, signal, socket, subprocess, sys, tempfile, time, urllib.request

T = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, T)
from freeport import PortLock, pick, wait_bound
P = argparse.ArgumentParser()
P.add_argument("kernel"); P.add_argument("outdir")
P.add_argument("--words", default="sdport=0 display=0 bench=0 blocklimit=1000")
P.add_argument("--steps", default="rytm-a4,a4-rytm,again")
P.add_argument("--expect-folders", action="store_true")
A = P.parse_args()
HTTP_FIXED = os.environ.get("HTTP_PORT")
GAP = float(os.environ.get("ATTACH_GAP", "20"))
HOLD = float(os.environ.get("HOLD", "30"))
WEB_WAIT = float(os.environ.get("WEB_WAIT", "300"))
HELLO = {"rytm": 0, "a4": 3, "rytm2": 0}   # a stock Rytm never says hello; the A4 companion says hello with id 3
# BOXES=rytm,rytm2: two Rytms (same type twice; run-sidecar.sh RYTM2_LINK) in place of the Rytm and the A4
SECOND = "rytm2" if os.environ.get("BOXES", "") == "rytm,rytm2" else "a4"
DEV = ("rytm", SECOND)          # the QEMU device ids run-sidecar.sh gives them
os.makedirs(A.outdir, exist_ok=True)
_lock = open(os.path.join(A.outdir, ".twobox-gate.lock"), "w")
try:
    fcntl.flock(_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
except OSError:
    sys.exit(f"another twobox-gate.py is using {A.outdir}; give each run its own OUTDIR")
SOCKS = tempfile.mkdtemp(prefix="twobox-")   # short paths (a unix socket path has ~107 bytes), one folder per run
LINKS = {b: os.path.join(SOCKS, b + ".link") for b in ("rytm", "a4", "rytm2")}
fails = []


class Step:
    def __init__(self, name, card):
        self.name, self.card = name, card
        self.d = os.path.join(A.outdir, name); os.makedirs(self.d, exist_ok=True)
        self.t0 = time.time(); self.log_f = open(os.path.join(self.d, "run.log"), "w")
        self.stubs, self.qemu, self.qmp = {}, None, None
        self.http = int(HTTP_FIXED) if HTTP_FIXED else None   # picked at boot, under the host-wide port lock
        self.qmp_path = os.path.join(SOCKS, name + ".qmp")
        self.status_f = open(os.path.join(self.d, "status.log"), "w"); self.last = None
        self.summary = {"step": self.name, "events": [], "http_port": self.http}

    def log(self, *a):
        s = f"{time.time() - self.t0:7.1f} " + " ".join(str(x) for x in a)
        print(f"[{self.name}] {s}", flush=True); self.log_f.write(s + "\n"); self.log_f.flush()

    def check(self, what, ok, detail=""):
        self.log(("PASS " if ok else "FAIL ") + what, detail)
        if not ok:
            fails.append(f"{self.name}: {what} {detail}")

    # the links
    def plug(self, box):
        n = sum(1 for e in self.summary["events"] if e[1] == "plug " + box) + 1
        f = open(os.path.join(self.d, f"stublink-{box}{n}.log"), "w")
        self.stubs[box] = subprocess.Popen([sys.executable, os.path.join(T, "stublink.py"), "unix:" + LINKS[box],
                                            "--hello", str(HELLO[box])],
                                           stdout=f, stderr=subprocess.STDOUT, start_new_session=True)
        self.event("plug " + box)

    def unplug(self, box):
        p = self.stubs.pop(box)
        os.killpg(p.pid, signal.SIGTERM); p.wait(timeout=10)
        self.event("unplug " + box)

    def event(self, what):
        self.log(what); self.summary["events"].append([round(time.time() - self.t0, 1), what])

    # QEMU
    def boot(self):
        if os.path.exists(self.qmp_path):
            os.unlink(self.qmp_path)
        second = {"RYTM2_LINK": "unix:" + LINKS["rytm2"], "A4_LINK": ""} if SECOND == "rytm2" else {"A4_LINK": "unix:" + LINKS["a4"]}
        env = dict(os.environ, LINK="unix:" + LINKS["rytm"], **second,
                   CARD=self.card, QMP=self.qmp_path, UART=os.path.join(self.d, "uart.log"),
                   GADGET_PORT="", WATCHDOG="off")
        words = f"{A.words} paircode=0".strip()
        with PortLock():   # pick and bind the web port before another run can pick it
            if not HTTP_FIXED:
                self.http = pick(18118)[0]
            self.summary["http_port"] = self.http
            self.qemu = subprocess.Popen([os.path.join(T, "..", "run-sidecar.sh"), A.kernel, words],
                                         env=dict(env, HTTP_PORT=str(self.http)),
                                         stdout=open(os.path.join(self.d, "qemu.out"), "w"),
                                         stderr=subprocess.STDOUT, start_new_session=True)
            self.event(f"qemu started (web on 127.0.0.1:{self.http})")
            wait_bound(self.http, 10, lambda: self.qemu.poll() is None)
        for _ in range(100):
            if os.path.exists(self.qmp_path):
                break
            time.sleep(0.1)
        s = socket.socket(socket.AF_UNIX); s.connect(self.qmp_path)
        self.qmp = s.makefile("rw"); self.qmp.readline(); self.qmp_call("qmp_capabilities")

    def qmp_call(self, cmd, args=None):
        self.qmp.write(json.dumps({"execute": cmd, "arguments": args or {}}) + "\n"); self.qmp.flush()
        while True:
            r = json.loads(self.qmp.readline())
            if "return" in r or "error" in r:
                return r

    def usb(self):
        """the boxes QEMU has attached (each device's `attached` property over QMP)"""
        got = []
        for b in DEV:
            r = self.qmp_call("qom-get", {"path": f"/machine/peripheral/{b}", "property": "attached"})
            if r.get("return") is True:
                got.append(b)
        open(os.path.join(self.d, "attached.log"), "a").write(f"{time.time() - self.t0:.1f} {got}\n")
        return sorted(got)

    def expect_usb(self, want, why):
        got = []
        for _ in range(20):        # a reconnect takes up to reconnect-ms (1 s)
            got = self.usb()
            if got == sorted(want):
                break
            time.sleep(0.5)
        self.check(f"QEMU shows {sorted(want) or 'no box'} {why}", got == sorted(want), f"(attached: {got})")

    def status(self):
        r = urllib.request.Request(f"http://127.0.0.1:{self.http}/status",
                                   headers={"Host": "10.0.2.15", "Origin": "http://10.0.2.15"})
        return json.loads(urllib.request.urlopen(r, timeout=5).read())

    def poll(self, secs):
        """poll /status for secs, logging each change; returns the last answer (or None)"""
        end, st = time.time() + secs, None
        while time.time() < end:
            if self.qemu.poll() is not None:
                self.log("qemu exited", self.qemu.returncode); return st
            try:
                st = self.status()
                pick = {k: st.get(k) for k in ("slots", "device", "link", "streaming", "rytmMidi", "usbmidi",
                                               "mirror", "slotFx") if k in st}
                s = json.dumps(pick, sort_keys=True)
                if s != self.last:
                    self.status_f.write(f"{time.time() - self.t0:.1f} {s}\n"); self.status_f.flush(); self.last = s
            except Exception:
                pass
            time.sleep(2)
        return st

    def web_up(self):
        end = time.time() + WEB_WAIT
        while time.time() < end and self.qemu.poll() is None:
            try:
                self.status(); self.event("web up"); return True
            except Exception:
                time.sleep(2)
        return False

    def finish(self):
        st = None
        try:
            st = self.status()
            json.dump(st, open(os.path.join(self.d, "status.json"), "w"), indent=1)
        except Exception as e:
            self.log("/status at the end:", e)
        self.check("/status answers at the end", st is not None)
        self.summary["slots_at_end"] = st.get("slots") if st else None
        if self.qemu.poll() is None:
            os.killpg(self.qemu.pid, signal.SIGTERM)
            try:
                self.qemu.wait(timeout=20)
            except subprocess.TimeoutExpired:
                os.killpg(self.qemu.pid, signal.SIGKILL); self.qemu.wait()
        for b in list(self.stubs):
            self.unplug(b)
        env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
        listing = {}
        for d in ("::/", "::/.rytm", "::/.a4", "::/.sidecar") + tuple(f"::/.rytm/{u}" for u in UIDS):
            r = subprocess.run(["mdir", "-a", "-b", "-i", self.card + "@@1M", d], capture_output=True, text=True,
                               env=env)
            listing[d[2:]] = r.stdout.split() if r.returncode == 0 else None
        self.summary["card"] = listing
        self.check("the card holds a readable FAT", listing["/"] is not None)
        folders = {f: listing[f] is not None for f in ("/.rytm", "/.a4", "/.sidecar")}
        self.summary["folders"] = folders
        self.log("card folders:", folders)
        if A.expect_folders and SECOND == "rytm2":
            have = [listing.get(f"/.rytm/{u}") is not None for u in UIDS]
            self.check(f"the card has a folder for each Rytm (/.rytm/{{{','.join(UIDS)}}})", bool(UIDS) and all(have))
        elif A.expect_folders:
            self.check("the card has /.rytm and /.a4", folders["/.rytm"] and folders["/.a4"])
        uart = open(os.path.join(self.d, "uart.log"), errors="replace").read() if os.path.exists(
            os.path.join(self.d, "uart.log")) else ""
        bad = [l for l in uart.splitlines() if re.search(r"panic|assert", l, re.I)]
        self.check("no panic/assert in the UART log", not bad, bad[0][:200] if bad else "")
        pat = re.compile(r"device: |slot|hello|claimed|attach|detach|release|/\.(rytm|a4|sidecar)", re.I)
        open(os.path.join(self.d, "uart-picked.log"), "w").write(
            "\n".join(l for l in uart.splitlines() if pat.search(l)) + "\n")
        self.summary["reboots"] = len(re.findall(r"engine: reboot:", uart))
        json.dump(self.summary, open(os.path.join(self.d, "summary.json"), "w"), indent=1)


UIDS = [u for u in os.environ.get("UIDS", "").split(",") if u]   # UID folders to list (two Rytms)


def two_orders(first, second):
    name = f"{first}-{second}"
    st = Step(name, os.path.join(A.outdir, name, "card.img"))
    subprocess.run([os.path.join(T, "mkcard.sh"), st.card], check=True, stdout=subprocess.DEVNULL)
    try:
        st.plug(first); st.boot()
        st.expect_usb([first], f"with only the {first} plugged")
        st.check("/status answers", st.web_up())
        st.poll(GAP)
        st.plug(second); st.expect_usb([first, second], f"after the {second} is plugged second")
        st.poll(HOLD)
        st.unplug(second); st.expect_usb([first], f"after the {second} is unplugged")
        st.poll(HOLD)
        st.plug(second); st.expect_usb([first, second], f"after the {second} is plugged back")
        st.poll(HOLD)
    finally:
        st.finish()
    return st


def again():
    card = os.path.join(A.outdir, f"rytm-{SECOND}", "card.img")
    if not os.path.exists(card):
        print(f"[again] needs the rytm-{SECOND} step's card; skipped"); return
    st = Step("again", card)
    try:
        st.plug("rytm"); st.plug(SECOND); st.boot()
        st.expect_usb(["rytm", SECOND], "with both plugged at boot")
        st.check("/status answers", st.web_up())
        st.poll(HOLD)
    finally:
        st.finish()


steps = A.steps.split(",")
try:
    for s in steps:
        if s == "rytm-a4":
            two_orders("rytm", "a4")
        elif s == "a4-rytm":
            two_orders("a4", "rytm")
        elif s == "rytm-rytm2" and SECOND == "rytm2":
            two_orders("rytm", "rytm2")
        elif s == "rytm2-rytm" and SECOND == "rytm2":
            two_orders("rytm2", "rytm")
        elif s == "again":
            again()
        else:
            sys.exit("unknown step " + s)
finally:
    shutil.rmtree(SOCKS, ignore_errors=True)
print("\n".join("FAIL " + f for f in fails))
print("TWO-BOX GATE (emulator):", "PASS" if not fails else f"FAIL ({len(fails)})")
sys.exit(1 if fails else 0)
