#!/bin/bash
# The Teensy relay's paths in the emulator, against an engine kernel (no Teensy, no box needed):
#   1. mirror: usb-rytm (stublink.py) and usb-teensy (teensystub.py --audio) behind a high-speed hub; the Pi mirrors
#      the Rytm through the relay, the computer enumerates it and sends the Rytm's vendor requests (forwarded
#      computer -> relay -> Pi -> box and back), then streams alt 3 both ways
#   2. flash: a made-up 6 KB program written through the Pi (POST /teensy/put, /teensy/flash): relay -> HalfKay ->
#      blocks -> start -> relay back with the expected build
#   3. faults: the same flash with each usb-teensy fault option; each must end the way the Pi reports it
# usage: sidecar/tools/teensy-gate.sh <kernel8-rpi4.img> <outdir> <sidecar-firmware checkout> ["engine words"]
#   (teensy_flash.py comes from the checkout's tools/). The cmdline gets paircode=0 (emulator only) and
#   mirrorfirst=relay (the Rytm goes to the relay; on a unit the A4 would take USB-C).
# Environment: QEMU, CPUS, HTTP_PORT (default: 18117 if free, else a free port, picked per boot), GADGET_PORT (default:
# 7400 if free, else a free port) as run-sidecar.sh. Kills only the processes it started.
# Several gates can run at once on one host, each with its own outdir: the stub links are unix sockets in a private
# temporary folder and the ports are picked per boot as above (each boot's are in its qemu.out). Give parallel runs
# different CPUS.
set -u
K=${1:?usage: teensy-gate.sh <kernel> <outdir> <sidecar-firmware> [words]}
O=${2:?outdir}
FW=${3:?sidecar-firmware checkout}
WORDS=${4:-"sdport=0 usbmidi=os:on usbreboot=1 usb=mirror mirror=a4 mirrorhold=60 relaydelay=10"}
WORDS="$WORDS paircode=0 mirrorfirst=relay"
T=$(cd "$(dirname "$0")" && pwd)
HTTP_FIXED=${HTTP_PORT:-} GADGET_FIXED=${GADGET_PORT:-}
SOCKS=$(mktemp -d "${TMPDIR:-/tmp}/teensy-gate.XXXXXX")
mkdir -p "$O"
python3 - "$O/test.hex" <<'PY'
import sys, random
random.seed(1)
def rec(addr, typ, data):
    b = [len(data), addr >> 8, addr & 0xff, typ] + list(data)
    return ":" + "".join("%02X" % x for x in b) + "%02X" % ((-sum(b)) & 0xff)
out = [rec(0, 4, b"\x60\x00")]
out += [rec(a, 0, bytes(random.randrange(256) for _ in range(16))) for a in range(0, 6 * 1024, 16)]
out.append(rec(0, 1, b""))
open(sys.argv[1], "w").write("\n".join(out) + "\n")
PY

pids=()
stop_all() { for p in "${pids[@]}"; do pkill -TERM -P "$p" 2>/dev/null; kill "$p" 2>/dev/null; done; wait 2>/dev/null; pids=(); }
trap 'stop_all; rm -rf "$SOCKS"' EXIT
web_up() { for _ in $(seq 1 60); do curl -s -m 3 "http://127.0.0.1:$HTTP_PORT/status" >/dev/null 2>&1 && return 0; sleep 3; done; return 1; }
boot() {  # boot <dir> <seconds> <teensy opts> [teensystub args...]
  local d=$1 s=$2 opts=$3; shift 3; mkdir -p "$d"
  python3 "$T/stublink.py" "unix:$SOCKS/rytm.link" > "$d/stublink.log" 2>&1 & pids+=($!)
  python3 "$T/teensystub.py" --port "unix:$SOCKS/teensy.link" --json "$d/teensy.json" "$@" > "$d/teensystub.log" 2>&1 & pids+=($!)
  sleep 0.5
  # pick the ports and start QEMU under the host-wide port lock, held until QEMU listens (freeport.py)
  exec {lk}>>"$(python3 -c 'import tempfile, os; print(os.path.join(tempfile.gettempdir(), "sidecar-emu-ports.lock"))')"
  flock "$lk"
  read -r HTTP_PORT GADGET_PORT < <(python3 "$T/freeport.py" "${HTTP_FIXED:-18117}" "${GADGET_FIXED:-7400}")
  [ -n "$HTTP_FIXED" ] && HTTP_PORT=$HTTP_FIXED; [ -n "$GADGET_FIXED" ] && GADGET_PORT=$GADGET_FIXED
  echo "web on 127.0.0.1:$HTTP_PORT, gadget on 127.0.0.1:$GADGET_PORT" > "$d/qemu.out"
  UART=$d/uart.log HTTP_PORT=$HTTP_PORT GADGET_PORT=$GADGET_PORT WATCHDOG=off TEENSY=unix:$SOCKS/teensy.link \
    TEENSY_OPTS=$opts LINK=unix:$SOCKS/rytm.link \
    timeout "$s" "$T/../run-sidecar.sh" "$K" "$WORDS" >> "$d/qemu.out" 2>&1 {lk}>&- & pids+=($!)   # QEMU must not keep the lock
  python3 "$T/freeport.py" --wait-bound "$HTTP_PORT" 10
  exec {lk}>&-
}
# The engine reaches usb=mirror at very different uptimes (about 35 s in the cloud, 2.5 min on a busy PC), so the
# steps wait for the relay's "box up" line rather than a fixed delay.
box_up() {  # box_up <dir>: waits up to BOX_WAIT s (default 360) for the relay to have the box, or for the Pi to see
  #                  the Teensy in HalfKay (start-halfkay: there is no relay until it has been flashed)
  for _ in $(seq 1 $(( ${BOX_WAIT:-360} / 2 ))); do
    grep -qE "teensy: usb=mirror: box up|teensyflash: a Teensy in its bootloader" "$1/uart.log" 2>/dev/null && return 0
    sleep 2
  done
  return 1
}
flash() {  # flash <dir> <expect>: prints done/failed line
  web_up || { echo "web never answered"; return; }
  box_up "$1" || { echo "the relay never had the box (nor the Pi a Teensy in HalfKay)"; return; }
  sleep 5
  python3 "$FW/tools/teensy_flash.py" "$O/test.hex" --host "127.0.0.1:$HTTP_PORT" --token none --expect "$2" \
    --timeout 120 > "$1/flash.log" 2>&1
  grep -E "^done|^failed|timed out" "$1/flash.log" | tail -1
}
fails=0
judge() { case $3 in "$2"*) echo "$1: PASS ($3)";; *) echo "$1: FAIL ($3; wanted: $2)"; fails=$((fails + 1));; esac; }

# STEPS="mirror flash ..." runs only those steps (default: all)
# 1. mirror + ctl forwarding + audio
if [[ " ${STEPS:-mirror} " == *" mirror "* ]]; then
boot "$O/mirror" 520 "" --audio --exit-after 500 --exit-after-shown 100
wait "${pids[2]}" 2>/dev/null; stop_all
python3 - "$O/mirror" <<'PY' | tee "$O/mirror/result.txt"
import json, sys
d = sys.argv[1]
try:
    st = json.load(open(d + "/teensy.json"))
except Exception:
    st = {}
ok = [a for a in st.get("answers", []) if a.get("status") == "ok"]
print("shown %d, enumerations %d, vendor answers ok %d of %d, IN packets %s, audio OUT blocks sent %d" % (
    st.get("shown", 0), st.get("enumerations", 0), len(ok), len(st.get("answers", [])), st.get("in_packets", {}),
    st.get("sent_O", 0)))
print("MIRROR:", "PASS" if st.get("shown") and len(ok) >= 3 and st.get("in_packets", {}).get("0x83", 0) > 1000 else "FAIL")
PY
grep -q "MIRROR: PASS" "$O/mirror/result.txt" || fails=$((fails + 1))
fi

# 2. a flash, 3. each fault
# The engine's own render-core guard reboots QEMU boots now and then (the known gate noise, not the relay's): a step
# whose boot had such a reboot and did not get its result runs once more, and says so.
run_flash() {  # run_flash <name> <teensy opts> <expect> <wanted result prefix>
  case " ${STEPS:-$1} " in *" $1 "*) ;; *) return;; esac
  boot "$O/$1" 600 "$2"
  r=$(flash "$O/$1" "$3")
  stop_all
  case $r in "$4"*) ;; *)
    if grep -q "engine: reboot:" "$O/$1/uart.log" 2>/dev/null; then
      echo "$1: the engine rebooted itself during the boot ($(grep -m1 -o 'engine: reboot: .*' "$O/$1/uart.log")): once more"
      mv "$O/$1" "$O/$1-try1"
      boot "$O/$1" 600 "$2"
      r=$(flash "$O/$1" "$3")
      stop_all
    fi;;
  esac
  judge "$1" "$4" "$r"
}
run_flash flash        "flash-build=abc1234"                    abc1234 "done: the relay is back with build abc1234"
run_flash drop-block   "flash-build=abc1234,drop-block=3"       abc1234 "failed: block 3 was not answered in time"
run_flash bl-ignore    "bl-ignore=on"                           abc1234 "failed: the Teensy did not come up in its bootloader"
run_flash bl-stay      "bl-stay=on"                             abc1234 "failed: the Teensy stayed in its bootloader"
run_flash wrong-build  "flash-build=zzz9999"                    abc1234 "failed: the relay is back with build zzz9999"
run_flash leave-quiet  "flash-build=abc1234,leave-quiet-ms=60"  abc1234 "done: the relay is back with build abc1234"
run_flash halfkay-boot "flash-build=abc1234,start-halfkay=on"   abc1234 "done: the relay is back with build abc1234"
[ -f "$O/leave-quiet/uart.log" ] && echo "quiet-leave transaction errors: $(grep -c 'Transfer error 4' "$O/leave-quiet/uart.log")"
for d in "$O"/*/; do echo "$(basename "$d"): panic/assert lines $(grep -ciE 'panic|assert' "$d/uart.log" 2>/dev/null)"; done
echo "TEENSY GATE: $([ $fails = 0 ] && echo PASS || echo "FAIL ($fails)")"
[ $fails = 0 ]
