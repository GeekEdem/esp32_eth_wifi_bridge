#!/usr/bin/env python3
"""Protocol test of dhcp_core.c with scapy as the independent DHCP
implementation: scapy builds the client messages and parses the server's
replies. dhcp_core.c is loaded as a shared library through ctypes.

    python3 dhcp_scapy_test.py        (needs: pip install scapy)
"""
import ctypes, os, subprocess, sys, tempfile, socket
from scapy.all import BOOTP, DHCP, raw

HERE = os.path.dirname(os.path.abspath(__file__))
lib_path = os.path.join(tempfile.mkdtemp(), 'libdhcp.so')
subprocess.check_call(['cc', '-shared', '-fPIC', '-O1', '-Wall', '-Wextra', '-I', f'{HERE}/../main',
                       f'{HERE}/../main/dhcp_core.c', '-o', lib_path])
lib = ctypes.CDLL(lib_path)

MAX_LEASES, HOST = 32, 24
class Lease(ctypes.Structure):
    _fields_ = [('used', ctypes.c_bool), ('reserved', ctypes.c_bool), ('bound', ctypes.c_bool),
                ('mac', ctypes.c_uint8 * 6), ('ip', ctypes.c_uint32), ('expires', ctypes.c_uint32),
                ('host', ctypes.c_char * HOST)]
class Core(ctypes.Structure):
    _fields_ = [('server_ip', ctypes.c_uint32), ('netmask', ctypes.c_uint32), ('pool_first', ctypes.c_uint32),
                ('pool_last', ctypes.c_uint32), ('router', ctypes.c_uint32), ('dns', ctypes.c_uint32),
                ('lease_s', ctypes.c_uint32), ('leases', Lease * MAX_LEASES), ('dirty', ctypes.c_bool)]

def ip(s): return int.from_bytes(socket.inet_aton(s), 'little')      # network order in a uint32
def ips(v): return socket.inet_ntoa(v.to_bytes(4, 'little'))
def mac(s): return bytes.fromhex(s.replace(':', ''))
MACA, MACB, MACC, MACD = '02:00:00:00:00:0a', '02:00:00:00:00:0b', '02:00:00:00:00:0c', '02:00:00:00:00:0d'

def new_core(first='192.168.77.100', last='192.168.77.200'):
    c = Core()
    lib.dhcp_core_init(ctypes.byref(c), ip('192.168.77.1'), ip('255.255.255.0'), ip(first), ip(last), 7200)
    c.router = ip('192.168.77.1')
    return c

NONE, BCAST, UNICAST = 0, 1, 2
def send(core, pkt, now):
    req = raw(pkt)
    resp = (ctypes.c_uint8 * 576)(); rlen = ctypes.c_size_t(); dst = ctypes.c_uint32()
    kind = lib.dhcp_core_handle(ctypes.byref(core), req, len(req), resp, ctypes.byref(rlen), ctypes.byref(dst), now)
    if kind == NONE:
        return None, None
    return BOOTP(bytes(resp[:rlen.value])), (ips(dst.value) if kind == UNICAST else 'broadcast')

def opts(reply):
    return {o[0]: o[1] if len(o) == 2 else o[1:] for o in reply[DHCP].options if isinstance(o, tuple)}

XID = [100]
def msg(m, kind, extra=(), ciaddr='0.0.0.0', giaddr='0.0.0.0', flags=0):
    XID[0] += 1
    return BOOTP(op=1, chaddr=mac(m) + b'\0' * 10, xid=XID[0], ciaddr=ciaddr, giaddr=giaddr, flags=flags) / \
           DHCP(options=[('message-type', kind), *extra, 'end'])

def check(cond, what):
    if not cond:
        print('FAIL:', what); sys.exit(1)

T = 1000
c = new_core()

# 1. DISCOVER -> OFFER, parsed by scapy
d = msg(MACA, 'discover', [('hostname', b'printer')])
r, to = send(c, d, T)
o = opts(r)
check(r.op == 2 and r.xid == d.xid and r.chaddr[:6] == mac(MACA), 'offer echoes xid/chaddr')
check(r.yiaddr == '192.168.77.100' and to == 'broadcast', f'offer .100 broadcast, got {r.yiaddr} {to}')
check(o['message-type'] == 2 and o['server_id'] == '192.168.77.1', 'offer type/server id')
check(o['lease_time'] == 7200 and o['renewal_time'] == 3600 and o['rebinding_time'] == 6300, f'lease times {o}')
check(o['subnet_mask'] == '255.255.255.0' and o['router'] == '192.168.77.1' and 'name_server' not in o, 'mask/router/no dns')
check(len(raw(r)) >= 300, 'reply padded to 300 bytes')

# 2. REQUEST (selecting) -> ACK, lease visible
r, to = send(c, msg(MACA, 'request', [('server_id', '192.168.77.1'), ('requested_addr', '192.168.77.100')]), T)
check(opts(r)['message-type'] == 5 and r.yiaddr == '192.168.77.100' and to == 'broadcast', 'ack .100')
check(ips(lib.dhcp_core_ip_of(ctypes.byref(c), mac(MACA), T)) == '192.168.77.100', 'ip_of A')
check(c.leases[0].host == b'printer', 'hostname kept')

# 3. second client gets the next address; asking for A's address is refused
r, _ = send(c, msg(MACB, 'discover'), T)
check(r.yiaddr == '192.168.77.101', 'B offered .101')
r, _ = send(c, msg(MACB, 'request', [('requested_addr', '192.168.77.100')]), T)
check(opts(r)['message-type'] == 6 and r.yiaddr == '0.0.0.0', 'init-reboot for a taken address -> NAK')
r, _ = send(c, msg(MACB, 'request', [('requested_addr', '10.0.0.5')]), T)
check(opts(r)['message-type'] == 6, 'foreign subnet -> NAK')

# 4. B takes another server's offer: silence, B's offer dropped
r, _ = send(c, msg(MACB, 'request', [('server_id', '192.168.77.254'), ('requested_addr', '192.168.77.9')]), T)
check(r is None, 'other server -> no reply')
r, _ = send(c, msg(MACD, 'discover'), T)
check(r.yiaddr == '192.168.77.101', f'dropped offer freed .101, got {r.yiaddr}')

# 5. renew by A: unicast ACK with ciaddr
r, to = send(c, msg(MACA, 'request', ciaddr='192.168.77.100'), T + 3600)
check(opts(r)['message-type'] == 5 and to == '192.168.77.100' and r.ciaddr == '192.168.77.100', 'renew unicast ack')

# 6. reservations
check(lib.dhcp_core_reserve(ctypes.byref(c), mac(MACC), ip('192.168.77.50')), 'reserve C .50 (outside pool)')
check(not lib.dhcp_core_reserve(ctypes.byref(c), mac(MACD), ip('192.168.77.50')), 'same address for another MAC refused')
check(not lib.dhcp_core_reserve(ctypes.byref(c), mac(MACD), ip('192.168.77.1')), 'server address refused')
check(not lib.dhcp_core_reserve(ctypes.byref(c), mac(MACD), ip('192.168.1.10')), 'other subnet refused')
r, _ = send(c, msg(MACC, 'discover'), T)
check(r.yiaddr == '192.168.77.50', 'C offered its reservation')
r, _ = send(c, msg(MACC, 'request', [('requested_addr', '192.168.77.120')]), T)
check(opts(r)['message-type'] == 6, 'C asking for another address -> NAK')
r, _ = send(c, msg(MACC, 'request', [('server_id', '192.168.77.1'), ('requested_addr', '192.168.77.50')]), T)
check(opts(r)['message-type'] == 5 and r.yiaddr == '192.168.77.50', 'C ack .50')
# pin A somewhere else: its renewal of .100 is refused, discover moves it
check(lib.dhcp_core_reserve(ctypes.byref(c), mac(MACA), ip('192.168.77.150')), 'reserve A .150')
r, _ = send(c, msg(MACA, 'request', ciaddr='192.168.77.100'), T + 4000)
check(opts(r)['message-type'] == 6, 'A renew of old address -> NAK')
r, _ = send(c, msg(MACA, 'discover'), T + 4000)
check(r.yiaddr == '192.168.77.150', 'A moved to .150')

# 7. restart: save/load keeps leases and reservations
buf = (ctypes.c_uint8 * 2048)()
n = lib.dhcp_core_save(ctypes.byref(c), buf, 2048, T + 4000)
c2 = new_core()
check(lib.dhcp_core_load(ctypes.byref(c2), buf, n, 50), 'load')
r, _ = send(c2, msg(MACA, 'request', [('requested_addr', '192.168.77.150')]), 60)
check(opts(r)['message-type'] == 5, 'after restart: reserved A init-reboot -> ACK')
r, _ = send(c2, msg(MACC, 'discover'), 60)
check(r.yiaddr == '192.168.77.50', 'after restart: C still .50')
check(ips(lib.dhcp_core_ip_of(ctypes.byref(c2), mac(MACD), 60)) == '0.0.0.0', 'unbound offer not saved')
check(lib.dhcp_core_unreserve(ctypes.byref(c2), mac(MACC)), 'unreserve C')
r, _ = send(c2, msg(MACC, 'discover'), 60)
check(r.yiaddr != '192.168.77.50', 'C back to the pool after unreserve')

# 8. small pool: exhaustion, release keeps affinity, expiry frees
c = new_core('192.168.77.100', '192.168.77.101')
for m_, want in ((MACA, '192.168.77.100'), (MACB, '192.168.77.101')):
    r, _ = send(c, msg(m_, 'discover'), T)
    send(c, msg(m_, 'request', [('server_id', '192.168.77.1'), ('requested_addr', r.yiaddr)]), T)
    check(r.yiaddr == want, f'{m_} -> {want}')
r, _ = send(c, msg(MACC, 'discover'), T)
check(r is None, 'pool exhausted -> no offer')
send(c, msg(MACA, 'release', ciaddr='192.168.77.100'), T + 10)
r, _ = send(c, msg(MACA, 'discover'), T + 20)
check(r.yiaddr == '192.168.77.100', 'released client gets the same address back')
r, _ = send(c, msg(MACC, 'discover'), T + 7300)
check(r is not None and r.yiaddr in ('192.168.77.100', '192.168.77.101'), 'expired lease reused')

# 9. DECLINE parks the address
c = new_core('192.168.77.100', '192.168.77.102')
r, _ = send(c, msg(MACA, 'discover'), T)
send(c, msg(MACA, 'request', [('server_id', '192.168.77.1'), ('requested_addr', r.yiaddr)]), T)
send(c, msg(MACA, 'decline', [('requested_addr', '192.168.77.100'), ('server_id', '192.168.77.1')]), T)
r, _ = send(c, msg(MACB, 'discover'), T + 1)
check(r.yiaddr != '192.168.77.100', 'declined address not offered')
r, _ = send(c, msg(MACA, 'discover'), T + 2)
check(r.yiaddr not in ('192.168.77.100',), 'declining client gets another address')

# 10. INFORM: unicast ACK, no address, no lease time
r, to = send(c, msg(MACD, 'inform', ciaddr='192.168.77.60'), T)
o = opts(r)
check(o['message-type'] == 5 and r.yiaddr == '0.0.0.0' and to == '192.168.77.60' and 'lease_time' not in o, 'inform')

# 11. ignored: relayed, replies, broken
check(send(c, msg(MACD, 'discover', giaddr='10.0.0.1'), T)[0] is None, 'relayed request ignored')
check(send(c, BOOTP(op=2, chaddr=mac(MACD)) / DHCP(options=[('message-type', 'discover'), 'end']), T)[0] is None, 'op=2 ignored')
check(send(c, BOOTP(op=1, chaddr=mac(MACD)), T)[0] is None, 'no DHCP options ignored')

# 12. probe (access-point mode): our DISCOVER as scapy sees it
lib.dhcp_probe_is_offer.restype = ctypes.c_bool
buf = (ctypes.c_uint8 * 600)()
n = lib.dhcp_probe_build(buf, 600, mac(MACA), ctypes.c_uint32(0x12345678))
d = BOOTP(bytes(buf[:n]))
check(d.op == 1 and d.xid == 0x12345678 and d.flags == 0x8000 and d.chaddr[:6] == mac(MACA), 'probe header')
check(opts(d)['message-type'] == 1 and d.ciaddr == '0.0.0.0', 'probe is a DISCOVER')
# ... answered by our own server core (a stand-in for the router's)
router = new_core()
r, _ = send(router, d, T)
check(r is not None and r.yiaddr.startswith('192.168.77.'), 'a server answers the probe')
def is_offer(pkt, xid=0x12345678, m=MACA, own='10.0.0.1'):
    b = pkt if isinstance(pkt, bytes) else raw(pkt); sid = ctypes.c_uint32()
    ok = lib.dhcp_probe_is_offer(b, len(b), mac(m), ctypes.c_uint32(xid), ip(own), ctypes.byref(sid))
    return ok, ips(sid.value)
check(is_offer(r) == (True, '192.168.77.1'), 'offer recognised, server id')
check(not is_offer(r, xid=0x12345679)[0], 'other xid')
check(not is_offer(r, m=MACB)[0], 'other chaddr')
check(not is_offer(r, own='192.168.77.1')[0], 'our own server is not "a server on the cable"')
ack = BOOTP(raw(r)); ack[DHCP].options = [('message-type', 'ack'), ('server_id', '192.168.77.1'), 'end']
check(not is_offer(ack)[0], 'ACK is not an offer')
noip = BOOTP(raw(r)); noip.yiaddr = '0.0.0.0'
check(not is_offer(noip)[0], 'offer without an address')
check(not is_offer(msg(MACA, 'discover'))[0], 'a request is not an offer')
for cut in range(0, len(raw(r))):
    check(not is_offer(raw(r)[:cut])[0] or cut >= 243, 'cut offer')     # needs the header, cookie and type (240 + 3)

print('all dhcp_core protocol tests passed (scapy)')
