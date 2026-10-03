#!/usr/bin/env python3
"""Routing-pitfall reproduction for the C3's RFC2217 server on real lwIP.

Needs root (a tap interface and iptables) and pyserial. Builds nothing: pass
the harness built by build.sh. Client A connects and sets RTS (the server then
streams a boot log, like the WT32 after a reset); right after that every packet
of A's connection is dropped in both directions, as when the RTS reset cuts the
route that carries the connection. Client B then tries to connect once a second
and the time until it gets in is printed. A second run checks that a client
that only reads (no commands for longer than the probe time) is not dropped.

Usage: sudo python3 run.py <harness> [--max S] [--quiet-client]
"""
import argparse, fcntl, os, socket, struct, subprocess, sys, threading, time
import serial

TAP, HOST_IP, LWIP_IP, PORT = 'tap0', '192.168.99.1', '192.168.99.2', 4000
URL = f'rfc2217://{LWIP_IP}:{PORT}'
PRE_KB = 200                                  # read_flash-like transfer before the reset


def make_tap():
    TUNSETIFF, TUNSETPERSIST, IFF_TAP, IFF_NO_PI = 0x400454ca, 0x400454cb, 0x0002, 0x1000
    fd = os.open('/dev/net/tun', os.O_RDWR)
    fcntl.ioctl(fd, TUNSETIFF, struct.pack('16sH', TAP.encode(), IFF_TAP | IFF_NO_PI))
    fcntl.ioctl(fd, TUNSETPERSIST, 1)
    os.close(fd)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    def addr(req, ip):
        fcntl.ioctl(s, req, struct.pack('16sH2s4s8s', TAP.encode(), socket.AF_INET, b'\0\0',
                                        socket.inet_aton(ip), b'\0' * 8))
    addr(0x8916, HOST_IP)                      # SIOCSIFADDR
    addr(0x891c, '255.255.255.0')              # SIOCSIFNETMASK
    flags = struct.unpack('16sH', fcntl.ioctl(s, 0x8913, struct.pack('16sH', TAP.encode(), 0)))[1]
    fcntl.ioctl(s, 0x8914, struct.pack('16sH', TAP.encode(), flags | 0x1 | 0x40))   # UP, RUNNING


def iptables(op, port):
    for chain, args in (('INPUT', ['-i', TAP, '--sport', str(PORT), '--dport', str(port)]),
                        ('OUTPUT', ['-o', TAP, '--sport', str(port), '--dport', str(PORT)])):
        subprocess.run(['iptables', op, chain, '-p', 'tcp'] + args + ['-j', 'DROP'], check=True)


def start(harness):
    env = dict(os.environ, PRECONFIGURED_TAPIF=TAP, PRE_KB=str(PRE_KB))
    p = subprocess.Popen([harness], env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    events = []
    def reader():
        for line in p.stdout:
            events.append((time.monotonic(), line.strip()))
    threading.Thread(target=reader, daemon=True).start()
    for _ in range(50):
        if any('ready' in e for _, e in events):
            break
        time.sleep(0.1)
    time.sleep(0.5)
    return p, events


def connect(timeout=3):
    return serial.serial_for_url(URL, timeout=0.2, do_not_open=False) if timeout else None


def pitfall(harness, limit):
    p, events = start(harness)
    try:
        a = connect()
        a_port = a._socket.getsockname()[1]
        got, t = 0, time.monotonic()
        while got < PRE_KB * 1024 and time.monotonic() - t < 10:
            got += len(a.read(65536))
        a.rts = True                           # the server starts the boot log
        time.sleep(0.05)
        iptables('-I', a_port)
        t0 = time.monotonic()
        try:
            while time.monotonic() - t0 < limit:
                try:
                    b = serial.serial_for_url(URL, timeout=0.2)
                    dt = time.monotonic() - t0
                    b.close()
                    break
                except serial.SerialException:
                    time.sleep(1)
            else:
                dt = None
        finally:
            iptables('-D', a_port)
        for t, e in events:
            if t >= t0 - 0.1:
                print(f'  +{t - t0:5.1f} s  {e.split(" ", 1)[1]}')
        return dt
    finally:
        p.kill()


def quiet_reader(harness, seconds):
    """A monitor that only reads: no commands, no data for longer than the probe time."""
    p, events = start(harness)
    try:
        a = connect()
        t0 = time.monotonic()
        while time.monotonic() - t0 < seconds:
            a.read(4096)
        dropped = any('drop' in e for _, e in events)
        probes = sum('probe' in e for _, e in events)
        a.close()
        return dropped, probes
    finally:
        p.kill()


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('harness')
    ap.add_argument('--max', type=float, default=17, help='longest acceptable time to the next client (s)')
    ap.add_argument('--limit', type=float, default=60)
    ap.add_argument('--quiet', type=float, default=35, help='how long the read-only client stays (s)')
    o = ap.parse_args()
    make_tap()
    dt = pitfall(o.harness, o.limit)
    print(f'next client after: {dt:.1f} s' if dt is not None else f'next client: none within {o.limit} s')
    dropped, probes = quiet_reader(o.harness, o.quiet)
    print(f'read-only client: {probes} probes answered, {"DROPPED" if dropped else "kept"} for {o.quiet:.0f} s')
    ok = dt is not None and dt <= o.max and not dropped
    print('ok' if ok else 'FAIL')
    sys.exit(0 if ok else 1)
