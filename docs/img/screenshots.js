// Screenshots of the web pages for the READMEs, in English and Ukrainian, taken
// against the test mocks (wt32/test/mock_wt32.py, shared/wifi_setup/test/mock_portal.py).
// The Firmware section shows the version, build time and SHA-256 read from the
// committed firmware/*-ota.bin images; the commit comes from $COMMIT (the one the
// images were built from; $C3_COMMIT for the C3 image if it differs) or HEAD.
// Usage, from the repository root: node docs/img/screenshots.js   (needs playwright)
const { chromium } = require('playwright');
const { spawn, execSync } = require('child_process');
const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..', '..');
const OUT = __dirname;
const W = 390;                                   // phone width
const sleep = ms => new Promise(r => setTimeout(r, ms));

// esp_app_desc_t at offset 32 of an app image
function buildInfo(bin, project, commit) {
  const b = fs.readFileSync(path.join(ROOT, bin));
  const str = (off, len) => b.subarray(off, off + len).toString('latin1').replace(/\0.*$/s, '');
  const d = 32;
  if (b.readUInt32LE(d) !== 0xABCD5432) throw new Error(`${bin}: no app description`);
  return {
    project, version: str(d + 16, 32), time: str(d + 80, 16), date: str(d + 96, 16), idf: str(d + 112, 32),
    elf: b.subarray(d + 144, d + 152).toString('hex'),
    commit: commit || process.env.COMMIT || execSync('git rev-parse --short=10 HEAD', { cwd: ROOT }).toString().trim(),
  };
}

function mock(args, port, cwd) {
  const p = spawn('python3', [...args.slice(0, 3), String(port), ...args.slice(3)], { cwd, stdio: 'inherit' });
  return p;
}

(async () => {
  const b = await chromium.launch(process.env.CHROMIUM ? { executablePath: process.env.CHROMIUM } : {});
  const shots = [];

  async function page(base, lang, build) {
    const ctx = await b.newContext({ viewport: { width: W, height: 800 }, deviceScaleFactor: 2, locale: lang === 'uk' ? 'uk-UA' : 'en-US',
      colorScheme: 'light' });
    const p = await ctx.newPage();
    await p.route('**/api/status', async route => {
      const r = await route.fetch();
      const j = await r.json();
      j.build = build;
      j.version = build.version;
      await route.fulfill({ response: r, json: j });
    });
    await p.goto(base);
    return p;
  }
  async function login(p) {
    await p.waitForSelector('#loginOv input');
    await p.fill('#loginOv input', '12345678'); await p.click('#loginOv button[type=submit]');
    await p.waitForSelector('#loginOv', { state: 'detached' });
    await p.evaluate(() => document.fonts.ready);
  }
  // one picture from the top of the first element to the bottom of the last
  async function shot(p, name, first, last) {
    await sleep(400);
    const a = await p.locator(first).boundingBox(), z = await p.locator(last || first).boundingBox();
    const y = Math.max(0, a.y - 8);
    await p.screenshot({ path: path.join(OUT, name), fullPage: true, clip: { x: 0, y, width: W, height: z.y + z.height + 8 - y } });
    shots.push(name);
  }

  // WT32
  const wt32Build = buildInfo('wt32/firmware/wt32-bridge-ota.bin', 'wt32_bridge');
  const example = fs.readFileSync(path.join(ROOT, 'wt32/main/main.c'), 'utf8')
    .match(/SCRIPT_EXAMPLE\[\] =\n([\s\S]*?);\n/)[1].split('\n').map(l => JSON.parse(l.trim())).join('');
  let port = 8870;
  for (const lang of ['en', 'uk']) {
    const wt = () => mock(['mock_wt32.py', '../main/page.html', '../../shared/wifi_setup/auth.js'], ++port, path.join(ROOT, 'wt32/test'));
    let m = wt(); await sleep(800);
    let base = `http://127.0.0.1:${port}`;
    let p = await page(base, lang, wt32Build);
    await p.waitForSelector('#loginOv input');
    await sleep(300);
    await p.screenshot({ path: path.join(OUT, `login_${lang}.png`), clip: { x: 0, y: 0, width: W, height: 520 } });
    shots.push(`login_${lang}.png`);
    await login(p);
    // the example script saved and running
    await p.request.post(`${base}/api/script?run=1`, { data: example, headers: { 'Content-Type': 'text/plain' } });
    await p.reload();
    // client mode: status and traffic; then the mode switch
    await p.waitForFunction(() => document.getElementById('devIp').textContent === '192.168.1.50');
    await shot(p, `client_${lang}.png`, 'h1', '#traffic');
    await p.check('input[value=client]');
    await shot(p, `mode_${lang}.png`, '#cfgClient >> xpath=ancestor::section');
    // script with its results
    await p.waitForFunction(() => document.querySelector('#script table')?.textContent.includes('192.168.1.50'), null, { timeout: 10000 });
    await shot(p, `script_${lang}.png`, '#script');
    await shot(p, `firmware_${lang}.png`, '#ota');
    // router: status and the DHCP list
    await p.request.post(`${base}/api/own`, { form: { ssid: 'WT32-Net', pass: 'wt32pass1', channel: '6', ip: '192.168.77.1' } });
    await p.request.post(`${base}/api/mode`, { form: { mode: 'own' } });
    await p.request.get(`${base}/_restart`);
    await p.reload();
    await p.waitForFunction(() => document.querySelectorAll('#leases tr').length === 2);
    await p.click('#leases tr:nth-child(1) button');
    await p.waitForFunction(() => document.querySelector('#leases tr:nth-child(1)').textContent.match(/pinned|закріплено/));
    await shot(p, `router_${lang}.png`, 'h1', '#dhcpSec');
    await p.context().close();
    m.kill();

    // C3 programmer
    m = mock(['mock_portal.py', '../../../tools/c3-programmer/main/portal.html', '../auth.js', '../../../tools/c3-programmer/main/i18n'],
      ++port, path.join(ROOT, 'shared/wifi_setup/test'));
    await sleep(800);
    base = `http://127.0.0.1:${port}`;
    p = await page(base, lang, buildInfo('tools/c3-programmer/firmware/c3-programmer-ota.bin', 'c3_programmer', process.env.C3_COMMIT));
    await login(p);
    await p.waitForFunction(() => document.getElementById('state').className === 'ok');
    await p.waitForFunction(() => document.querySelector('#ssid option[value="Home"]'));
    await shot(p, `c3_${lang}.png`, 'h1', '#boot >> xpath=ancestor::section');
    await p.context().close();
    m.kill();
  }
  await b.close();
  console.log('screenshots:', shots.join(' '));
})().catch(e => { console.error(e); process.exit(1); });
