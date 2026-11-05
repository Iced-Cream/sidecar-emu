# Run device_add / device_del on a QEMU QMP unix socket, written HMP-style:
#   qmp.py mon.sock "device_add usb-hub,id=hub1,bus=usb-bus.1,port=1" "device_del hub1"
import json, socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); f = s.makefile('rw')
def call(cmd, args=None):
    f.write(json.dumps({'execute': cmd, 'arguments': args or {}}) + '\n'); f.flush()
    while True:
        r = json.loads(f.readline())
        if 'return' in r or 'error' in r: return r
f.readline(); call('qmp_capabilities')
for c in sys.argv[2:]:
    verb, _, rest = c.partition(' ')
    if verb == 'device_add':
        drv, *props = rest.split(',')
        a = {'driver': drv}
        for p in props:
            k, v = p.split('=', 1)
            a[k] = {'on': True, 'off': False}.get(v, v)
        r = call('device_add', a)
    elif verb == 'device_del':
        r = call('device_del', {'id': rest.strip()})
    else:
        sys.exit('only device_add and device_del: ' + c)
    print(r.get('error', {}).get('desc', 'ok'))
