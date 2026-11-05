#!/bin/bash
# usage: hubfail.sh <kernel> <outdir> <cycles>: a hub whose status-change endpoint always errors, unplugged at varied times
K=$1; O=$2; N=${3:-30}; S=$(cd "$(dirname "$0")" && pwd); Q=${QEMU:-$(cd "$S/../.." && pwd)/build/qemu-system-aarch64}
mkdir -p "$O"; cd "$O"; rm -f mon.sock uart.log
timeout 1500 $Q -M raspi4b -kernel "$K" -append "ip=10.0.2.15 netmask=255.255.255.0" -serial file:uart.log -display none \
  -qmp unix:mon.sock,server,nowait -nic user,model=bcm2838-genet,hostfwd=tcp:127.0.0.1:18080-:80 > qemu.out 2>&1 &
QP=$!
w() { python3 -I -c "import time; time.sleep($1)"; }
up() { curl -s -m 3 -o /dev/null -w "%{http_code}" http://127.0.0.1:18080/status; }
for i in $(seq 30); do [ "$(up)" = 200 ] && break; w 1; done
echo "boot status $(up)"
hmp() { python3 -I $S/qmp.py mon.sock "$@" >/dev/null; }
DELAYS=(0.3 0.6 0.9 1.2 1.5 1.8 2.1 2.4 2.7 3.0 3.3 3.6 4.2 5.0 0.45)
fails=0
for c in $(seq $N); do
  d=${DELAYS[$(( (c-1) % ${#DELAYS[@]} ))]}
  hmp "device_add usb-hub,id=hub1,bus=usb-bus.1,port=1,fail-intr=on"; w 1.0; w $d
  hmp "device_del hub1"; w 2.5
  st=$(up); alive=$(kill -0 $QP 2>/dev/null && echo alive || echo dead)
  [ "$st" = 200 ] && [ $alive = alive ] || fails=$((fails+1))
  echo "cycle $c (unplug +$d s): status $st, qemu $alive, re-armed so far $(grep -c 're-armed' uart.log)"
done
echo "FAILED CYCLES $fails"
kill $QP 2>/dev/null
