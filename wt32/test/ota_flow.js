// Browser test of the firmware update section (ota.js) against mock_wt32.py:
// the merged image is refused, the OTA image updates (progress, restart,
// re-login, other slot), and a rollback is reported.
// Usage: node ota_flow.js <port> <merged.bin> <ota.bin>
const { chromium } = require('playwright');
const assert = require('assert');
(async () => {
  const [port, merged, ota] = process.argv.slice(2);
  const base = `http://127.0.0.1:${port}`;
  const b = await chromium.launch(process.env.CHROMIUM ? { executablePath: process.env.CHROMIUM } : {});
  const p = await b.newPage({ locale: 'en-US' })   /* the texts below are English, whatever the host's language */;
  const errors = []; p.on('pageerror', e => errors.push(e.message));
  const login = async () => {
    await p.waitForSelector('#loginOv', { timeout: 20000 });
    await p.fill('#loginOv input', '12345678'); await p.click('#loginOv button[type=submit]');
    await p.waitForSelector('#loginOv', { state: 'detached' });
  };
  const msg = () => p.textContent('#ota > div:last-child');

  await p.goto(base); await login();
  await p.waitForFunction(() => document.querySelector('#ota .mute').textContent.includes('partition ota_0'));

  // 1. merged image for 0x0 is refused, nothing restarts
  await p.setInputFiles('#ota input[type=file]', merged);
  await p.click('#ota button');
  await p.waitForFunction(() => document.querySelector('#ota > div:last-child').textContent.includes('-ota.bin'));

  // 2. the OTA image: written, restart, log in again, running from ota_1
  await p.setInputFiles('#ota input[type=file]', ota);
  await p.click('#ota button');
  await p.waitForFunction(() => document.querySelector('#ota > div:last-child').textContent.includes('Restarting'));
  await login();
  await p.waitForFunction(() => document.querySelector('#ota > div:last-child').textContent.includes('Updated to version '), null, { timeout: 30000 });
  assert((await p.textContent('#ota .mute')).includes('ota_1'));

  // 3. rollback: the device comes back from the same slot
  await fetch(`${base}/_rollback`);
  await p.setInputFiles('#ota input[type=file]', ota);
  await p.click('#ota button');
  await login();
  await p.waitForFunction(() => document.querySelector('#ota > div:last-child').textContent.includes('the previous one is back'), null, { timeout: 30000 });

  assert.deepStrictEqual(errors, [], 'no page errors');
  console.log('ota section: refuse / update / rollback ok');
  await b.close();
})().catch(e => { console.error(e); process.exit(1); });
