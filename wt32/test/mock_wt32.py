# Mock of the WT32 web API (setup_portal core + web.c): login, status per
# mode (client / own = router / ap = access point), /api/mode, /api/own, /api/wifi. "Restart" applies the saved mode.
import http.server, json, os, secrets, struct, time, urllib.parse, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(sys.argv[2])), 'test'))
import mock_i18n
HERE = os.path.dirname(os.path.abspath(__file__))
L = mock_i18n.Langs([os.path.join(HERE, '..', 'main', 'i18n'), os.path.join(HERE, '..', '..', 'shared', 'script_berry', 'i18n')])
PAGE = open(sys.argv[1], 'rb').read()
AUTHJS = open(sys.argv[2], 'rb').read()
OTAJS = open(sys.argv[2].replace('auth.js', 'ota.js'), 'rb').read()
SCRIPTJS = open(sys.argv[2].replace('wifi_setup/auth.js', 'script_berry/script.js'), 'rb').read()
EXAMPLE = "# Example\nprint('hi')\n"
st = {'pw': '12345678', 'sessions': set(), 'mode': 'client', 'saved_mode': 'client', 'log': [],
      'dhcp': [{"mac": "00:11:22:33:44:55", "ip": "192.168.77.100", "reserved": False, "left": 5400, "host": "GS-2406T"},
               {"mac": "AA:BB:CC:00:00:01", "ip": "192.168.77.101", "reserved": False, "left": 7000, "host": "phone"}],
      'script': {'text': None, 'state': 'stopped', 'error': '', 'autostart': False, 'console': [], 'outputs': [], 'runs': 0},
      'ota': {'running': 'ota_0', 'down_until': 0, 'rollback': False, 'version': '0.2.0'},
      'own': {'ssid': 'WT32-1A2B', 'pass': '', 'channel': 6, 'ip': '192.168.77.1'}, 'wifi': {'ssid': ''}}
CLIENT = {"state":"connected","ssid":"Home","host":"wt32","apSsid":"WT32-Setup-1A2B","ap":False,"ip":"","rssi":-55,
 "version":"0.1.0","wifiUp":True,"eth":True,"devMac":"00:11:22:33:44:55","devIp":"192.168.1.50","toWifi":[10,1000],
 "toEth":[12,1400],"dropWifiDown":0,"dropEthDown":0,"txErrWifi":0,"txErrEth":0,"foreign":0,"ipv6Dropped":0,
 "dhcpRewrites":4,"mgmtIp":"192.168.1.50","mgmtPort":28480,"mgmtFrames":[3,4],"mgmtTxErr":0,"mgmtFlows":1,"mgmtEvictions":0}
OWN = {"state":"setup","ssid":"","host":"","apSsid":"","ap":False,"ip":"","rssi":0,"version":"0.1.0",
 "eth":True,"apClients":1,"devMac":"00:11:22:33:44:55","devIp":"192.168.77.100"}
AP = {"state":"setup","ssid":"","host":"","apSsid":"","ap":False,"ip":"","rssi":0,"version":"0.1.0",
 "eth":True,"apClients":2,"devMac":"","devIp":"","apIp":"192.168.1.23","uplink":"dhcp","gw":"192.168.1.1"}
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def sid(self):
        for part in (self.headers.get('Cookie') or '').split(';'):
            k, _, v = part.strip().partition('=')
            if k == 'sid': return v
    def send(self, code, body, ctype='application/json', hdrs=()):
        b = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code); self.send_header('Content-Type', ctype); self.send_header('Content-Length', len(b))
        for k, v in hdrs: self.send_header(k, v)
        self.end_headers(); self.wfile.write(b)
    def form(self):
        n = int(self.headers.get('Content-Length') or 0)
        return {k: v[0] for k, v in urllib.parse.parse_qs(self.rfile.read(n).decode(), keep_blank_values=True).items()}
    def do_GET(self):
        if self.path == '/': return self.send(200, PAGE, 'text/html; charset=utf-8')
        if self.path == '/auth.js': return self.send(200, AUTHJS, 'application/javascript')
        if self.path == '/ota.js': return self.send(200, OTAJS, 'application/javascript')
        if self.path == '/script.js': return self.send(200, SCRIPTJS, 'application/javascript')
        if self.path.startswith('/i18n.json'): return self.send(200, L.response(self.path))
        if self.path == '/_restart':                       # test hook: apply saved mode
            st['mode'] = st['saved_mode']; return self.send(200, {"ok": True})
        if self.path == '/_fallback':                      # test hook: no DHCP on the cable
            st['uplink'] = 'fallback'; return self.send(200, {"ok": True})
        if self.path == '/_setupboot': st['setup_boot'] = True; return self.send(200, {"ok": True})
        if self.path == '/_log': return self.send(200, st['log'])
        if self.path == '/_rollback': st['ota']['rollback'] = True; return self.send(200, {"ok": True})
        if time.time() < st['ota']['down_until']:                # "restarting"
            self.close_connection = True; return self.send(503, b'', 'text/plain')
        if self.sid() not in st['sessions']: return self.send(401, L.err('err.loginRequired', auth=False))
        if self.path == '/api/status':
            o = st['own']
            if st['mode'] == 'own': base = dict(OWN, ownIp=o['ip'], dhcp=st['dhcp'])
            elif st['mode'] == 'ap':
                base = dict(AP)
                if st.get('uplink') == 'fallback': base.update(uplink='fallback', apIp=o['ip'], gw='')
            else: base = dict(CLIENT)
            o2 = st['ota']
            base.update(version=o2['version'], build=dict(mock_i18n.BUILD, project='wt32_bridge', version=o2['version']), ota={"supported": True, "running": o2['running'],
                        "maxSize": 0x1E0000, "probation": False})
            if st.get('setup_boot'): base['setupBoot'] = True
            base.update(mode=st['mode'], defaultPassword=True,
                        own={"ssid":o['ssid'],"channel":o['channel'],"ip":o['ip'],"passSet":len(o['pass'])>=8})
            return self.send(200, base)
        if self.path == '/api/scan': return self.send(200, [{"ssid":"Home","rssi":-50,"open":False}])
        if self.path == '/api/script':
            sc = st['script']; txt = sc['text'] if sc['text'] is not None else EXAMPLE
            return self.send(200, txt.encode(), 'text/plain; charset=utf-8', hdrs=[('X-Script-Saved', '1' if sc['text'] is not None else '0')])
        if self.path.startswith('/api/script/state'):
            sc = st['script']; since = int(urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query).get('since', ['0'])[0])
            return self.send(200, {"ok": True, "state": sc['state'], "autostart": sc['autostart'], "crashDisabled": False,
                "runs": sc['runs'], "mem": {"used": 9000, "peak": 12000, "limit": 40960}, "heap": 90000,
                "saved": len(sc['text'] or ''), "maxLen": 32768, "error": sc['error'], "outputs": sc['outputs'],
                "seq": len(sc['console']), "console": sc['console'][since:]})
        self.send(404, {})
    def script_run(self):
        sc = st['script']; sc['runs'] += 1; sc['console'].append(f"-- start #{sc['runs']}")
        if 'while true' in (sc['text'] or ''):                    # stands in for the runtime's time limit
            sc['state'] = 'error'; sc['error'] = 'timeout_error: script code ran too long'
            sc['console'].append('! ' + sc['error']); return
        sc['state'] = 'running'; sc['error'] = ''; sc['outputs'] = [["Mode", "client"], ["Ethernet", "link up"], ["Device IP", "192.168.1.50"], ["Free memory", "87 KB"]]
        sc['console'].append('script started')
    def do_POST(self):
        if self.path.startswith('/api/script') and self.path.split('?')[0] == '/api/script':
            n = int(self.headers.get('Content-Length') or 0); body = self.rfile.read(n).decode()
            if self.sid() not in st['sessions']: return self.send(401, L.err('err.loginRequired', auth=False))
            st['script']['text'] = body; st['script']['state'] = 'stopped'
            if 'run=1' in self.path: self.script_run()
            return self.send(200, {"ok": True})
        if self.path == '/api/ota':                                # mirrors portal_ota.c checks
            n = int(self.headers.get('Content-Length') or 0); body = self.rfile.read(n)
            if self.sid() not in st['sessions']: return self.send(401, L.err('err.loginRequired', auth=False))
            if len(body) < 288 or body[0] != 0xE9 or struct.unpack_from('<I', body, 32)[0] != 0xABCD5432:
                return self.send(400, L.err('err.thisIsNotAnUpdate'))
            ver = body[48:80].split(b'\0')[0].decode()
            o = st['ota']; o['down_until'] = time.time() + 3
            if not o['rollback']:
                o['running'] = 'ota_1' if o['running'] == 'ota_0' else 'ota_0'; o['version'] = ver
            st['sessions'] = set()                                 # RAM sessions die with the restart
            return self.send(200, {"ok":True,"version":ver})
        f = self.form()
        if self.path == '/api/login':
            if f.get('password') != st['pw']: return self.send(401, L.err('err.wrongPassword'))
            s = secrets.token_hex(16); st['sessions'].add(s)
            return self.send(200, {"ok":True,"defaultPassword":True}, hdrs=[('Set-Cookie', f'sid={s}; Path=/')])
        if self.sid() not in st['sessions']: return self.send(401, L.err('err.loginRequired', auth=False))
        st['log'].append([self.path, f])
        if self.path == '/api/own':
            p = f.get('pass') or st['own']['pass']
            if not (8 <= len(p) <= 63): return self.send(400, L.err('err.theNetworkPasswordMustBe'))
            st['own'].update({'ssid': f['ssid'], 'pass': p, 'channel': int(f['channel']), 'ip': f['ip']})
            return self.send(200, {"ok":True})
        if self.path == '/api/mode':
            if f['mode'] not in ('client', 'own', 'ap'): return self.send(400, L.err('err.unknownMode'))
            if f['mode'] != 'client' and len(st['own']['pass']) < 8:
                return self.send(400, L.err('err.saveTheWt32NetworkSettings'))
            st['saved_mode'] = f['mode']; return self.send(200, {"ok":True})
        if self.path == '/api/wifi': st['wifi']['ssid'] = f['ssid']; return self.send(200, {"ok":True})
        if self.path in ('/api/dhcp/reserve', '/api/dhcp/unreserve'):
            if st['mode'] != 'own': return self.send(400, L.err('err.addressPinningWorksInRouter'))
            m = f.get('mac', '').upper(); hit = [l for l in st['dhcp'] if l['mac'] == m]
            if self.path.endswith('/unreserve'):
                if not hit or not hit[0]['reserved']: return self.send(400, L.err('err.noAddressIsPinnedFor'))
                hit[0]['reserved'] = False; return self.send(200, {"ok": True})
            if not f.get('ip', '').startswith('192.168.77.'): return self.send(400, L.err('err.theAddressIsOutsideThe'))
            if hit: hit[0].update(reserved=True, ip=f['ip'])
            else: st['dhcp'].append({"mac": m, "ip": f['ip'], "reserved": True, "left": -1, "host": ""})
            return self.send(200, {"ok": True})
        if self.path == '/api/script/run': self.script_run(); return self.send(200, {"ok": True})
        if self.path == '/api/script/stop':
            st['script']['state'] = 'stopped'; st['script']['console'].append('-- stopped'); return self.send(200, {"ok": True})
        if self.path == '/api/script/autostart': st['script']['autostart'] = f.get('on') == '1'; return self.send(200, {"ok": True})
        return self.send(200, {"ok":True})
http.server.ThreadingHTTPServer(('127.0.0.1', int(sys.argv[3])), H).serve_forever()
