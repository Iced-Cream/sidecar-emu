#!/bin/bash
# Boot a Sidecar engine kernel on the raspi4b machine with the Sidecar devices.
# usage: sidecar/run-sidecar.sh <kernel8-rpi4.img> ["engine words"]
# Environment (all optional):
#   QEMU         qemu-system-aarch64 to run (default: build/ in the repo root)
#   HTTP_PORT    host port forwarded to the engine's port 80 (default 18080)
#   LINK         host:port of analog-emu's --ob-link (or tools/stublink.py);
#                empty = no usb-rytm (default 127.0.0.1:7301)
#   GADGET_PORT  TCP port for the USB-C gadget's raw MIDI, one client at a time,
#                e.g. elkflash --port tcp:127.0.0.1:7400; empty = gadget off (default 7400)
#                With TEENSY or A4_LINK set, a high-speed usb-hub goes on the VL805's USB 2
#                port (as the Pi 4's on-board hub) with usb-rytm on its port 1
#                and usb-teensy on its port 2 (usb-a4 on port 3)
#   TEENSY       host:port of sidecar/tools/teensystub.py (the computer behind
#                the relay); empty = no usb-teensy (default empty). TEENSY=none
#                plugs usb-teensy with no computer behind it
#   TEENSY_OPTS  more usb-teensy properties, comma-separated (e.g.
#                flash-build=abc1234,drop-block=5; hw/usb/dev-teensy.c)
#   A4_LINK      host:port of a second box's link (analog-emu's A4 --ob-link, or
#                tools/stublink.py): plugs usb-a4 (1935:0b47) on port 3 of the
#                high-speed hub, with usb-rytm on port 1 (default empty = no A4).
#                The boxes attach when their links connect and detach when they
#                close, so starting and stopping the links sets the attach order
#   CARD         raw SD card image for EMMC2 (tools/mkcard.sh makes one); the
#                engine's folders (/.rytm, /.a4, /.sidecar) land there
#                (default empty = no card)
#   QMP          unix socket path for a QMP monitor (tools/qmp.py, or
#                tools/twobox-gate.py); default empty = none
#   UART         file for the engine's UART log (default uart.log)
#   WATCHDOG     off = the Pi's watchdog never fires on a missed pat; reboots
#                the guest asks for (a short timeout) still happen (default on)
#   CPUS         host CPUs to pin QEMU to (taskset list, e.g. 0-1); QEMU's
#                threads hand its global lock to each other thousands of times
#                a second, and on a many-core host (WSL) that is far slower
#                than on 2 CPUs. Empty = no pinning (default)
#   EXTRA        more QEMU arguments
# LINK, A4_LINK and TEENSY also take unix:PATH, a unix socket the stub listens on
# (stublink.py unix:PATH, teensystub.py --port unix:PATH): a path per run needs no
# free port, so several runs can share a host (tools/twobox-gate.py, tools/teensy-gate.sh)
set -euo pipefail
K=${1:?usage: run-sidecar.sh <kernel8-rpi4.img> [\"engine words\"]}
WORDS=${2:-}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
HTTP_PORT=${HTTP_PORT:-18080}
LINK=${LINK-127.0.0.1:7301}
GADGET_PORT=${GADGET_PORT-7400}
UART=${UART:-uart.log}
args=(-M raspi4b -kernel "$K" -append "ip=10.0.2.15 netmask=255.255.255.0 $WORDS"
      -serial "file:$UART" -serial null -display none
      -nic "user,model=bcm2838-genet,hostfwd=tcp:127.0.0.1:$HTTP_PORT-:80")
if [ "${WATCHDOG:-on}" = off ]; then
  args+=(-global bcm2835-powermgt.watchdog=off)
fi
if [ -n "$GADGET_PORT" ]; then
  args+=(-chardev "socket,id=gm,host=127.0.0.1,port=$GADGET_PORT,server=on,wait=off"
         -global dwc2-usb.gadget-chardev=gm)
fi
if [ -n "${CARD:-}" ]; then
  args+=(-drive "if=sd,format=raw,file=$CARD")
fi
if [ -n "${QMP:-}" ]; then
  args+=(-qmp "unix:$QMP,server=on,wait=off")
fi
# chardev socket address for host:port or unix:PATH
addr() { case $1 in unix:*) echo "path=${1#unix:}";; *) echo "host=${1%:*},port=${1##*:}";; esac; }
TEENSY=${TEENSY:-}
A4_LINK=${A4_LINK:-}
rport=() tport=() aport=()
if [ -n "$TEENSY" ] || [ -n "$A4_LINK" ]; then
  args+=(-device usb-hub,id=pihub,bus=usb-bus.1,port=1,high-speed=on,ports=4)
  rport=(,bus=usb-bus.1,port=1.1) tport=(,bus=usb-bus.1,port=1.2) aport=(,bus=usb-bus.1,port=1.3)
fi
if [ -n "$A4_LINK" ]; then
  args+=(-chardev "socket,id=al,$(addr "$A4_LINK"),reconnect-ms=1000"
         -device "usb-a4,id=a4,chardev=al${aport[*]}")
fi
# RYTM2_LINK: a second Rytm on the hub's port 3 instead of the A4 (same type twice; RYTM2_UID its unit ID, the
# vendor request 0's answer, default 0x9f003fa2; the first Rytm's RYTM_UID, default the device's own)
RYTM2_LINK=${RYTM2_LINK:-}
if [ -n "$RYTM2_LINK" ]; then
  [ -n "$TEENSY" ] || [ -n "$A4_LINK" ] || { args+=(-device usb-hub,id=pihub,bus=usb-bus.1,port=1,high-speed=on,ports=4); rport=(,bus=usb-bus.1,port=1.1); }
  args+=(-chardev "socket,id=r2l,$(addr "$RYTM2_LINK"),reconnect-ms=1000"
         -device "usb-rytm,id=rytm2,chardev=r2l,uid=${RYTM2_UID:-0x9f003fa2},bus=usb-bus.1,port=1.3")
fi
if [ -n "$LINK" ]; then
  args+=(-chardev "socket,id=rl,$(addr "$LINK"),reconnect-ms=1000"
         -device "usb-rytm,id=rytm,chardev=rl${rport[*]}${RYTM_UID:+,uid=$RYTM_UID}")
fi
if [ "$TEENSY" = none ]; then
  args+=(-device "usb-teensy${tport[*]}${TEENSY_OPTS:+,$TEENSY_OPTS}")
elif [ -n "$TEENSY" ]; then
  args+=(-chardev "socket,id=tl,$(addr "$TEENSY"),reconnect-ms=1000"
         -device "usb-teensy,chardev=tl${tport[*]}${TEENSY_OPTS:+,$TEENSY_OPTS}")
fi
# shellcheck disable=SC2086
pin=()
if [ -n "${CPUS:-}" ]; then
  pin=(taskset -c "$CPUS")
fi
exec "${pin[@]}" "$QEMU" "${args[@]}" ${EXTRA:-}
