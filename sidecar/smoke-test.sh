#!/bin/bash
# Smoke test, no firmware involved. Two boots of an engine kernel:
#   1. a QEMU USB keyboard on the VL805's USB 2 port: /status over GENET, keyboard enumerates
#   2. usb-rytm on that port, linked to tools/stublink.py, plus the USB-C gadget:
#      /status, usb-rytm enumerates, the gadget's endpoints come up
# usage: sidecar/smoke-test.sh <kernel8-rpi4.img>
set -uo pipefail
K=${1:?usage: smoke-test.sh <kernel8-rpi4.img>}
S=$(cd "$(dirname "$0")" && pwd)
O=$(mktemp -d)
export HTTP_PORT=${HTTP_PORT:-18080}
STUB_PORT=${STUB_PORT:-7301}
fail=0
wait_status() {
  local st=000
  for _ in $(seq 60); do
    st=$(curl -s -m 2 -o /dev/null -w "%{http_code}" "http://127.0.0.1:$HTTP_PORT/status")
    [ "$st" = 200 ] && break
    sleep 1
  done
  sleep 5
  if [ "$st" = 200 ]; then echo "PASS $1: /status 200"; else echo "FAIL $1: /status ($st)"; fail=1; fi
}
check() { if grep -q "$3" "$1"; then echo "PASS $2"; else echo "FAIL $2"; fail=1; fi; }

UART=$O/uart-kbd.log LINK= GADGET_PORT= EXTRA="-device usb-kbd,bus=usb-bus.1,port=1" \
  "$S/run-sidecar.sh" "$K" > "$O/qemu-kbd.out" 2>&1 &
QP=$!
wait_status "boot 1"
check "$O/uart-kbd.log" "usb-kbd enumerates" "QEMU USB Keyboard"
kill $QP 2>/dev/null; wait $QP 2>/dev/null

python3 -I "$S/tools/stublink.py" "$STUB_PORT" > "$O/stublink.log" 2>&1 &
SP=$!
UART=$O/uart-rytm.log LINK=127.0.0.1:$STUB_PORT GADGET_PORT=${GADGET_PORT:-7400} \
  "$S/run-sidecar.sh" "$K" > "$O/qemu-rytm.out" 2>&1 &
QP=$!
wait_status "boot 2"
check "$O/uart-rytm.log" "usb-rytm enumerates" "Analog Rytm MKII"
check "$O/uart-rytm.log" "USB-C gadget endpoints up" "Endpoint(s) activated"
kill $QP $SP 2>/dev/null; wait 2>/dev/null
echo "logs in $O"
exit $fail
