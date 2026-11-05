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
#   UART         file for the engine's UART log (default uart.log)
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
if [ -n "$GADGET_PORT" ]; then
  args+=(-chardev "socket,id=gm,host=127.0.0.1,port=$GADGET_PORT,server=on,wait=off"
         -global dwc2-usb.gadget-chardev=gm)
fi
if [ -n "$LINK" ]; then
  args+=(-chardev "socket,id=rl,host=${LINK%:*},port=${LINK##*:},reconnect-ms=1000"
         -device usb-rytm,chardev=rl)
fi
# shellcheck disable=SC2086
exec "$QEMU" "${args[@]}" ${EXTRA:-}
