#!/usr/bin/env python3
"""Routing-pitfall reproduction for the C3's RFC2217 server on real lwIP.

Needs root (a tap interface and iptables) and pyserial. Builds nothing: pass
the harness built by build.sh. Client A connects and sets RTS (the server then
streams a boot log, like the WT32 after a reset); right after that every packet
of A's connection is dropped in both directions, as when the RTS reset cuts the
route that carries the connection. Client B then tries to connect once a second
and the time until it gets in is printed. A second run checks that a client
that only reads (no commands for longer than the probe time) is not dropped.

--half-open adds, right after the cut, a connection attempt that was given up
before the handshake completed and whose SYN-ACK goes unanswered: what the bench
showed when esptool was run again while the dead client was still held (the
retry's socket was closed when the C3's SYN-ACK came, and Windows' firewall sends
no RST). lwIP keeps it half-open (SYN-RCVD) for ~20 s in the listen queue; with a
backlog of 1 that blocks every other client meanwhile (C3 0.4.2-0.4.4).

Usage: sudo python3 run.py <harness> [--max S] [--half-open]
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


HALF_OPEN_PORT = 40404


def half_open(op):
    """'-I': a connection attempt given up before the handshake completes; its
    SYN-ACK (and lwIP's retransmissions of it) never reach the host, so the host
    sends no RST either. '-D': remove the rule again."""
    subprocess.run(['iptables', op, 'INPUT', '-i', TAP, '-p', 'tcp', '--sport', str(PORT),
                    '--dport', str(HALF_OPEN_PORT), '-j', 'DROP'], check=True)
    if op == '-I':
        s = socket.socket()
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((HOST_IP, HALF_OPEN_PORT))
        s.setblocking(False)
        s.connect_ex((LWIP_IP, PORT))
        time.sleep(0.3)
        s.close()


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


def pitfall(harness, limit, with_half_open=False):
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
            if with_half_open:
                half_open('-I')
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
            if with_half_open:
                half_open('-D')
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
    ap.add_argument('--max', type=float, default=None,
                    help='longest acceptable time to the next client (s); default 17, with --half-open 18.5')
    ap.add_argument('--limit', type=float, default=60)
    ap.add_argument('--quiet', type=float, default=35, help='how long the read-only client stays (s)')
    ap.add_argument('--half-open', action='store_true',
                    help='add a given-up connection attempt (half-open in lwIP) right after the cut')
    o = ap.parse_args()
    if o.max is None:
        # With the half-open connection, client B's stale attempts from the dead
        # period fill the rest of the queue, so B gets in with its next SYN
        # retransmission (~17 s on the host); with a backlog of 1 it waits for
        # the half-open connection to expire (~20 s).
        o.max = 18.5 if o.half_open else 17
    make_tap()
    dt = pitfall(o.harness, o.limit, o.half_open)
    print(f'next client after: {dt:.1f} s' if dt is not None else f'next client: none within {o.limit} s')
    dropped, probes = quiet_reader(o.harness, o.quiet)
    print(f'read-only client: {probes} probes answered, {"DROPPED" if dropped else "kept"} for {o.quiet:.0f} s')
    ok = dt is not None and dt <= o.max and not dropped
    print('ok' if ok else 'FAIL')
    sys.exit(0 if ok else 1)
