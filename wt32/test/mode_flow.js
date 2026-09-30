// Browser test of the WT32 page against mock_wt32.py: client status, switching
// to the router mode, to the access point mode (DHCP and fallback) and back. Usage: node mode_flow.js <port>
const { chromium } = require('playwright');
const assert = require('assert');
(async () => {
  const base = `http://127.0.0.1:${process.argv[2]}`;
  const b = await chromium.launch(process.env.CHROMIUM ? { executablePath: process.env.CHROMIUM } : {});
  const p = await b.newPage();
  const errors = []; p.on('pageerror', e => errors.push(e.message));
  const log = async () => (await (await fetch(`${base}/_log`)).json()).map(([u, f]) => u + ' ' + JSON.stringify(f));

  await p.goto(base);
  await p.fill('#loginOv input', '12345678'); await p.click('#loginOv button[type=submit]');
  await p.waitForSelector('#loginOv', { state: 'detached' });

  // client mode status
  await p.waitForFunction(() => document.getElementById('devIp').textContent.startsWith('192.168.1.50'));
  assert(await p.isVisible('#tblClient') && !(await p.isVisible('#tblOwn')) && await p.isVisible('#traffic'));
  assert(await p.isChecked('input[value=client]') && await p.isVisible('#cfgClient'));

  // switch to own: short password is refused on the page
  await p.check('input[value=own]');
  assert(await p.isVisible('#cfgOwn') && !(await p.isVisible('#cfgClient')));
  assert.strictEqual(await p.inputValue('#ownSsidIn'), 'WT32-1A2B');
  await p.fill('#ownPassIn', 'short'); await p.click('#saveOwn');
  await p.waitForFunction(() => document.getElementById('msg').textContent.includes('8–63'));
  await p.fill('#ownSsidIn', 'Printer-Net'); await p.fill('#ownPassIn', 'labelprint1');
  await p.selectOption('#ownCh', '11'); await p.fill('#ownIpIn', '192.168.50.1');
  await p.click('#saveOwn');
  await p.waitForFunction(() => document.getElementById('msg').textContent.includes('http://192.168.50.1'));
  let l = await log();
  assert.deepStrictEqual(l.slice(-2), [
    '/api/own {"ssid":"Printer-Net","pass":"labelprint1","channel":"11","ip":"192.168.50.1"}',
    '/api/mode {"mode":"own"}']);

  // after the restart the page shows the own network
  await fetch(`${base}/_restart`);
  await p.reload();
  await p.waitForFunction(() => document.getElementById('ownDevIp').textContent === '192.168.77.100');
  assert(await p.isVisible('#tblOwn') && !(await p.isVisible('#tblClient')) && !(await p.isVisible('#traffic')));
  assert(await p.isChecked('input[value=own]'));
  assert.strictEqual(await p.textContent('#ownSsid'), 'Printer-Net');
  assert((await p.textContent('#ownPassHint')).includes('keep the current password'));

  // router -> access point: same network settings, the IP field is the fallback
  await p.check('input[value=ap]');
  assert(await p.isVisible('#cfgOwn') && !(await p.isVisible('#cfgClient')));
  assert.strictEqual(await p.textContent('#ownIpLbl'), 'Fallback WT32 address');
  assert((await p.textContent('#modeHelp')).includes("The WT32's cable goes into a router"));
  assert.strictEqual(await p.inputValue('#ownSsidIn'), 'Printer-Net');
  await p.click('#saveOwn');
  await p.waitForFunction(() => document.getElementById('msg').textContent.includes('wt32.local'));
  l = await log();
  assert.deepStrictEqual(l.slice(-2), [
    '/api/own {"ssid":"Printer-Net","pass":"","channel":"11","ip":"192.168.50.1"}',
    '/api/mode {"mode":"ap"}']);
  await fetch(`${base}/_restart`);
  await p.reload();
  await p.waitForFunction(() => document.getElementById('apIp').textContent.includes('192.168.1.23'));
  assert(await p.isVisible('#tblAp') && !(await p.isVisible('#tblOwn')) && !(await p.isVisible('#dhcpSec')) && !(await p.isVisible('#traffic')));
  assert(await p.isChecked('input[value=ap]'));
  assert.strictEqual(await p.getAttribute('#apIp a', 'href'), 'http://192.168.1.23/');
  assert((await p.textContent('#apIp')).includes('from the router'));
  assert.strictEqual(await p.textContent('#apGw'), '192.168.1.1');
  assert.strictEqual(await p.textContent('#apSsidT'), 'Printer-Net');
  await fetch(`${base}/_fallback`);
  await p.waitForFunction(() => document.getElementById('apIp').textContent.includes('fallback'));
  assert.strictEqual(await p.getAttribute('#apIp a', 'href'), 'http://192.168.50.1/');
  assert.strictEqual(await p.textContent('#apGw'), '—');
  assert((await p.textContent('#hint')).includes('DHCP'));
  await p.check('input[value=own]');
  assert.strictEqual(await p.textContent('#ownIpLbl'), 'WT32 address');

  // back to client: mode without restart first, then Wi-Fi (which restarts)
  await p.check('input[value=client]');
  await p.waitForFunction(() => document.querySelector('#ssid option[value="Home"]'));
  await p.fill('#pass', 'homepass1'); await p.click('#saveClient');
  await p.waitForFunction(() => document.getElementById('msg').textContent.includes('“Home”'));
  l = await log();
  assert.deepStrictEqual(l.slice(-2), [
    '/api/mode {"mode":"client","restart":"0"}',
    '/api/wifi {"ssid":"Home","pass":"homepass1"}']);

  // a static address from another network: said on the page, the setup AP stays on
  await fetch(`${base}/_restart`);
  await p.reload();
  await p.waitForFunction(() => document.getElementById('devIp').textContent === '192.168.1.50 (DHCP)');
  await fetch(`${base}/_othernet`);
  await p.waitForFunction(() => document.getElementById('hint').textContent.includes('static address, 192.168.1.77'));
  assert((await p.textContent('#devIp')).includes('192.168.1.77 (static)'));
  assert((await p.textContent('#mgmt')).includes('another network'));
  assert((await p.textContent('#hint')).includes('WT32-Setup-1A2B'));

  // one-time setup start from the button: the page says so
  assert(!(await p.textContent('#hint')).includes('One-time'));
  await fetch(`${base}/_setupboot`);
  await p.waitForFunction(() => document.getElementById('hint').textContent.includes('One-time setup start'));

  assert.deepStrictEqual(errors, [], 'no page errors');
  await p.screenshot({ path: process.env.SHOT || '/dev/null', fullPage: true }).catch(() => {});
  console.log('wt32 page: mode flow ok');
  await b.close();
})().catch(e => { console.error(e); process.exit(1); });
