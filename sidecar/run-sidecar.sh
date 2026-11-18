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
#                With TEENSY set, a high-speed usb-hub goes on the VL805's USB 2
#                port (as the Pi 4's on-board hub) with usb-rytm on its port 1
#                and usb-teensy on its port 2
#   TEENSY       host:port of sidecar/tools/teensystub.py (the computer behind
#                the relay); empty = no usb-teensy (default empty). TEENSY=none
#                plugs usb-teensy with no computer behind it
#   TEENSY_OPTS  more usb-teensy properties, comma-separated (e.g.
#                flash-build=abc1234,drop-block=5; hw/usb/dev-teensy.c)
#   UART         file for the engine's UART log (default uart.log)
#   WATCHDOG     off = the Pi's watchdog never fires on a missed pat; reboots
#                the guest asks for (a short timeout) still happen (default on)
#   CPUS         host CPUs to pin QEMU to (taskset list, e.g. 0-1); QEMU's
#                threads hand its global lock to each other thousands of times
#                a second, and on a many-core host (WSL) that is far slower
#                than on 2 CPUs. Empty = no pinning (default)
#   EXTRA        more QEMU arguments
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
TEENSY=${TEENSY:-}
rport=() tport=()
if [ -n "$TEENSY" ]; then
  args+=(-device usb-hub,id=pihub,bus=usb-bus.1,port=1,high-speed=on,ports=4)
  rport=(,bus=usb-bus.1,port=1.1) tport=(,bus=usb-bus.1,port=1.2)
fi
if [ -n "$LINK" ]; then
  args+=(-chardev "socket,id=rl,host=${LINK%:*},port=${LINK##*:},reconnect-ms=1000"
         -device "usb-rytm,chardev=rl${rport[*]}")
fi
if [ "$TEENSY" = none ]; then
  args+=(-device "usb-teensy${tport[*]}${TEENSY_OPTS:+,$TEENSY_OPTS}")
elif [ -n "$TEENSY" ]; then
  args+=(-chardev "socket,id=tl,host=${TEENSY%:*},port=${TEENSY##*:},reconnect-ms=1000"
         -device "usb-teensy,chardev=tl${tport[*]}${TEENSY_OPTS:+,$TEENSY_OPTS}")
fi
# shellcheck disable=SC2086
pin=()
if [ -n "${CPUS:-}" ]; then
  pin=(taskset -c "$CPUS")
fi
exec "${pin[@]}" "$QEMU" "${args[@]}" ${EXTRA:-}
