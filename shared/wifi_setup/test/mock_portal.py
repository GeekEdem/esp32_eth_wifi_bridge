# Mock of the portal API (behaviour mirrors setup_portal.c / portal_auth.c)
import http.server, json, secrets, urllib.parse, sys
PAGE = open(sys.argv[1], 'rb').read()
AUTHJS = open(sys.argv[2], 'rb').read()
OTAJS = open(sys.argv[2].replace('auth.js', 'ota.js'), 'rb').read()
state = {'pw': '12345678', 'default': True, 'sessions': set(), 'fails': 0}
STATUS = {"state":"connected","ssid":"Home","host":"wt32","apSsid":"WT32-Setup-1A2B","ap":True,"ip":"","rssi":-55,
 "version":"0.1.0","wifiUp":True,"eth":True,"devMac":"00:11:22:33:44:55","devIp":"192.168.1.50","toWifi":[10,1000],
 "toEth":[12,1400],"dropWifiDown":0,"dropEthDown":0,"txErrWifi":0,"txErrEth":0,"foreign":0,"ipv6Dropped":0,
 "dhcpRewrites":4,"mgmtIp":"192.168.1.50","mgmtPort":28480,"mgmtFrames":[3,4],"mgmtTxErr":0,"mgmtFlows":1,"mgmtEvictions":0,
 "port":4000,"client":False,"en":False,"boot":False,
 "ota":{"supported":True,"running":"ota_0","maxSize":1966080,"probation":False}}
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def sid(self):
        c = self.headers.get('Cookie') or ''
        for part in c.split(';'):
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
    def authed(self): return self.sid() in state['sessions']
    def do_GET(self):
        if self.path == '/': return self.send(200, PAGE, 'text/html; charset=utf-8')
        if self.path == '/auth.js': return self.send(200, AUTHJS, 'application/javascript')
        if self.path == '/ota.js': return self.send(200, OTAJS, 'application/javascript')
        if not self.authed(): return self.send(401, {"ok":False,"auth":False,"message":"Потрібен вхід"})
        if self.path == '/api/status': return self.send(200, dict(STATUS, defaultPassword=state['default']))
        if self.path == '/api/scan': return self.send(200, [{"ssid":"Home","rssi":-50,"open":False}])
        self.send(404, {})
    def do_POST(self):
        f = self.form()
        if self.path == '/api/login':
            if f.get('password') != state['pw']:
                return self.send(401, {"ok":False,"auth":False,"message":"Невірний пароль"})
            s = secrets.token_hex(16); state['sessions'].add(s)
            return self.send(200, {"ok":True,"defaultPassword":state['default']}, hdrs=[('Set-Cookie', f'sid={s}; Path=/; HttpOnly; SameSite=Strict')])
        if not self.authed(): return self.send(401, {"ok":False,"auth":False,"message":"Потрібен вхід"})
        if self.path == '/api/password':
            if f.get('old') != state['pw']: return self.send(400, {"ok":False,"message":"Поточний пароль невірний"})
            state.update(pw=f['new'], default=False); state['sessions'] = {self.sid()}
            return self.send(200, {"ok":True})
        if self.path == '/api/logout':
            state['sessions'].discard(self.sid()); return self.send(200, {"ok":True})
        return self.send(200, {"ok":True})
http.server.ThreadingHTTPServer(('127.0.0.1', int(sys.argv[3])), H).serve_forever()
