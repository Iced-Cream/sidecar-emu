# A stand-in for rytm-emu's Overbridge link server (oblink.rs protocol), for testing usb-rytm without firmware.
import socket, struct, sys, time, threading, math
port = int(sys.argv[1]) if len(sys.argv) > 1 else 7301
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); srv.bind(('127.0.0.1', port)); srv.listen(1)
st = dict(A=0, M=0, H=None, Mbytes=b'')
def reader(c):
    buf = b''
    def need(n):
        nonlocal buf
        while len(buf) < n:
            d = c.recv(65536)
            if not d: raise EOFError
            buf += d
        r, buf = buf[:n], buf[n:]; return r
    try:
        while True:
            k = need(1)
            if k == b'H': st['H'] = need(2); c.sendall(b'H\x01' + bytes([st['H'][1] & 1]))
            elif k == b'A': n = struct.unpack('<I', need(4))[0]; need(n * 48); st['A'] += n
            elif k == b'M':
                n = struct.unpack('<H', need(2))[0]; d = need(n); st['M'] += n; st['Mbytes'] += d
                if b'\xf0\x7d\x52\x54\x4f' in st['Mbytes']: print('HELLO ANSWER', st['Mbytes'].hex(), flush=True); st['Mbytes'] = b''
                if b'\xf0\x7e\x7f\x06\x01\xf7' in st['Mbytes']:   # identity request -> a reply with a long SysEx
                    st['Mbytes'] = b''
                    rep = bytes([0xf0, 0x7e, 0x00, 0x06, 0x02, 0x00, 0x20, 0x3c] + [i % 128 for i in range(40)] + [0xf7])
                    c.sendall(b'M' + struct.pack('<H', len(rep)) + rep)
            else: print('bad', k); return
    except EOFError: pass
while True:
    c, a = srv.accept(); print('host connected', a, flush=True)
    threading.Thread(target=reader, args=(c,), daemon=True).start()
    c.sendall(b'N' + struct.pack('<I', 512))
    t0 = time.time(); sent = 0; ph = 0
    try:
        while True:
            time.sleep(0.01)
            due = int((time.time() - t0) * 48000) - sent
            if due > 0:
                fr = b''.join(struct.pack('<12i', *([int(1e9 * math.sin(2 * math.pi * 1000 * (sent + i) / 48000))] * 2 + [0] * 10)) for i in range(due))
                c.sendall(b'R' + struct.pack('<I', due) + fr + b'N' + struct.pack('<I', due)); sent += due
            if 'h' not in st and time.time() - t0 > 6:
                st['h'] = 1
                hel = bytes([0xf8, 0xf0, 0x7d, 0x52, 0x54, 0x4f, 0x01, 0x00, 0x07, 0x01, 0xf7, 0xfe])   # clock, the device hello, active sensing
                c.sendall(b'M' + struct.pack('<H', 4) + hel[:4] + b'M' + struct.pack('<H', len(hel) - 4) + hel[4:])
                print('sent hello', flush=True)
            if int(time.time() - t0) != int(time.time() - t0 - 0.01):
                print('t=%d A=%d M=%d hello=%s' % (time.time() - t0, st['A'], st['M'], st['H']), flush=True)
    except OSError: print('host gone', flush=True)
