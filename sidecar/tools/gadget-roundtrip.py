# Send a MIDI identity request into the USB-C gadget's raw MIDI port and wait
# for a SysEx reply (with tools/stublink.py as the Rytm, the reply is its
# 49-byte stand-in answer). usage: gadget-roundtrip.py [port] [tries]
import socket, sys, time
port = int(sys.argv[1]) if len(sys.argv) > 1 else 7400
tries = int(sys.argv[2]) if len(sys.argv) > 2 else 3
ok = 0
s = socket.create_connection(('127.0.0.1', port), timeout=5)
for n in range(tries):
    s.sendall(bytes([0xf0, 0x7e, 0x7f, 0x06, 0x01, 0xf7]))
    buf = b''; t0 = time.time()
    while time.time() - t0 < 5:
        try:
            d = s.recv(4096)
        except socket.timeout:
            break
        if not d: break
        buf += d
        i = buf.find(b'\xf0\x7e')
        if i >= 0 and buf.find(b'\xf7', i) > i:
            rep = buf[i:buf.index(b'\xf7', i) + 1]
            print('reply %d: %d bytes in %.2f s' % (n + 1, len(rep), time.time() - t0)); ok += 1; break
    else:
        print('reply %d: none' % (n + 1))
print('round trips %d/%d' % (ok, tries))
sys.exit(0 if ok == tries else 1)
