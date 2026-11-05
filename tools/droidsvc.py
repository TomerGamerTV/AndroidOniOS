#!/usr/bin/env python3
"""Dev stand-in for the iOS app's guest service host (macOS testing).

The guest opens qemu pipes over vsock (host port 5000); QEMU's virtio-vsock-host
device forwards each connection to this unix socket. The first bytes are the
NUL-terminated pipe name, e.g. "pipe:qemud:sensors".

usage: droidsvc.py <unix socket path>
"""
import math
import os
import socket
import struct
import sys
import threading
import time

SENSOR_MASK = sum(1 << b for b in (0, 1, 2, 3, 5, 7, 9, 10, 17))
BOOT_PROPS = [
    'qemu.sf.lcd_density=420',
    'qemu.hw.mainkeys=0',
    'qemu.timezone=Asia/Jerusalem',
]


def log(*a):
    print('[droidsvc]', *a, flush=True)


def qemud_send(sock, payload):
    if isinstance(payload, str):
        payload = payload.encode()
    sock.sendall(b'%04x' % len(payload) + payload)


def recv_exact(sock, n):
    buf = b''
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError
        buf += chunk
    return buf


def qemud_recv(sock):
    hdr = recv_exact(sock, 4)
    return recv_exact(sock, int(hdr, 16)).decode(errors='replace')


def svc_sensors(sock):
    state = {'delay': 0.2, 'run': True, 'started': False}

    def pump():
        while state['run']:
            try:
                qemud_send(sock, 'acceleration:%.4f:%.4f:%.4f' % (0.0, 9.80665, 0.0))
                qemud_send(sock, 'gyroscope:0:0:0')
                qemud_send(sock, 'magnetic-field:0:22:-40')
                qemud_send(sock, 'orientation:0:0:0')
                qemud_send(sock, 'proximity:5')
                qemud_send(sock, 'pressure:1013.25')
                qemud_send(sock, 'sync:%d' % int(time.time() * 1e6))
            except OSError:
                return
            time.sleep(state['delay'])

    try:
        while True:
            msg = qemud_recv(sock)
            if msg == 'list-sensors':
                qemud_send(sock, str(SENSOR_MASK))
            elif msg.startswith('set:') and msg.endswith(':1') and not state['started']:
                state['started'] = True
                threading.Thread(target=pump, daemon=True).start()
            elif msg.startswith('set-delay:'):
                state['delay'] = max(0.01, int(msg.split(':')[1]) / 1000.0)
            log('sensors <-', msg)
    finally:
        state['run'] = False


def svc_boot_properties(sock):
    msg = qemud_recv(sock)
    log('boot-properties <-', msg)
    for p in BOOT_PROPS:
        qemud_send(sock, p)
    qemud_send(sock, b'\0')
    while sock.recv(4096):
        pass


def svc_misc(sock):
    while True:
        (n,) = struct.unpack('<i', recv_exact(sock, 4))
        msg = recv_exact(sock, n).rstrip(b'\0').decode(errors='replace')
        log('QemuMiscPipe <-', msg)
        sock.sendall(struct.pack('<i', 0))


def nmea_checksum(s):
    c = 0
    for ch in s:
        c ^= ord(ch)
    return '%02X' % c


def svc_gps(sock):
    lat, lon = 32.0853, 34.7818  # Tel Aviv
    while True:
        t = time.gmtime()
        hhmmss = time.strftime('%H%M%S', t)
        ddmmyy = time.strftime('%d%m%y', t)

        def fmt(v, deg_digits):
            d = int(abs(v))
            m = (abs(v) - d) * 60
            return '%0*d%07.4f' % (deg_digits, d, m)
        gga = 'GPGGA,%s.00,%s,%s,%s,%s,1,08,1.0,10.0,M,0.0,M,,' % (
            hhmmss, fmt(lat, 2), 'N' if lat >= 0 else 'S', fmt(lon, 3), 'E' if lon >= 0 else 'W')
        rmc = 'GPRMC,%s.00,A,%s,%s,%s,%s,0.0,0.0,%s,,' % (
            hhmmss, fmt(lat, 2), 'N' if lat >= 0 else 'S', fmt(lon, 3), 'E' if lon >= 0 else 'W', ddmmyy)
        try:
            for s in (gga, rmc):
                sock.sendall(('$%s*%s\r\n' % (s, nmea_checksum(s))).encode())
        except OSError:
            return
        time.sleep(1)


SERVICES = {
    'pipe:qemud:sensors': svc_sensors,
    'pipe:qemud:boot-properties': svc_boot_properties,
    'pipe:QemuMiscPipe': svc_misc,
    'pipe:qemud:gps': svc_gps,
}


def handle(sock):
    name = b''
    try:
        while not name.endswith(b'\0'):
            ch = sock.recv(1)
            if not ch:
                return
            name += ch
        name = name[:-1].decode(errors='replace')
        log('open', name)
        fn = SERVICES.get(name)
        if fn:
            fn(sock)
        else:
            while sock.recv(4096):  # unknown service: stay open, ignore
                pass
    except (ConnectionError, OSError, ValueError) as e:
        log('closed', name, type(e).__name__)
    finally:
        sock.close()


def main():
    path = sys.argv[1]
    if os.path.exists(path):
        os.unlink(path)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(path)
    srv.listen(16)
    log('listening on', path)
    while True:
        s, _ = srv.accept()
        threading.Thread(target=handle, args=(s,), daemon=True).start()


if __name__ == '__main__':
    main()
