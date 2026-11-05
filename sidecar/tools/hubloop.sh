#!/bin/bash
# usage: hubloop.sh <kernel> <outdir> <cycles>
K=$1; O=$2; N=${3:-10}; S=$(cd "$(dirname "$0")" && pwd); Q=${QEMU:-$(cd "$S/../.." && pwd)/build/qemu-system-aarch64}
mkdir -p "$O"; cd "$O"; rm -f mon.sock uart.log
timeout 900 $Q -M raspi4b -kernel "$K" -append "ip=10.0.2.15 netmask=255.255.255.0" -serial file:uart.log -display none \
  -qmp unix:mon.sock,server,nowait -nic user,model=bcm2838-genet,hostfwd=tcp:127.0.0.1:18080-:80 > qemu.out 2>&1 &
QP=$!
w() { python3 -I -c "import time; time.sleep($1)"; }
up() { curl -s -m 3 -o /dev/null -w "%{http_code}" http://127.0.0.1:18080/status; }
for i in $(seq 30); do [ "$(up)" = 200 ] && break; w 1; done
echo "boot status $(up)"
hmp() { python3 -I $S/qmp.py mon.sock "$@" >/dev/null; }
for c in $(seq $N); do
  hmp "device_add usb-hub,id=hub1,bus=usb-bus.1,port=1"; w 3
  hmp "device_add usb-kbd,id=kbd1,bus=usb-bus.1,port=1.1"; w 4
  L0=$(wc -l < uart.log)
  hmp "device_del hub1"; w 4
  echo "cycle $c: status $(up), qemu $(kill -0 $QP 2>/dev/null && echo alive || echo dead), log lines $(wc -l < uart.log)"
done
kill $QP 2>/dev/null
