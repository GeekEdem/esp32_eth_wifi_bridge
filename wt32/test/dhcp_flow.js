// Browser test of the DHCP section (own network) against mock_wt32.py:
// leases listed with the cable device marked, pin/unpin, manual pin, errors.
// Usage: node dhcp_flow.js <port>   (switches the mock to the own network)
const { chromium } = require('playwright');
const assert = require('assert');
(async () => {
  const base = `http://127.0.0.1:${process.argv[2]}`;
  const b = await chromium.launch(process.env.CHROMIUM ? { executablePath: process.env.CHROMIUM } : {});
  const p = await b.newPage();
  const errors = []; p.on('pageerror', e => errors.push(e.message));
  const wait = fn => p.waitForFunction(fn, null, { timeout: 10000 });

  await p.goto(base);
  await p.fill('#loginOv input', '12345678'); await p.click('#loginOv button');
  await p.waitForSelector('#loginOv', { state: 'detached' });
  // own network needs saved settings first, then a "restart"
  await p.check('input[value=own]');
  await p.fill('#ownPassIn', 'labelprint1'); await p.click('#saveOwn');
  await wait(() => document.getElementById('msg').textContent.includes('перезапускається'));
  await fetch(`${base}/_restart`);
  await p.reload();

  await wait(() => document.querySelectorAll('#leases tr').length === 2);
  assert(await p.isVisible('#dhcpSec'));
  const first = await p.textContent('#leases tr:nth-child(1)');
  assert(first.includes('GS-2406T (на кабелі)') && first.includes('192.168.77.100') && first.includes('ще 90 хв'), first);

  await p.click('#leases tr:nth-child(1) button');               // pin the printer
  await wait(() => document.querySelector('#leases tr:nth-child(1)').textContent.includes('закріплено'));
  assert((await p.textContent('#leases tr:nth-child(1) button')) === 'Відкріпити');
  await p.click('#leases tr:nth-child(1) button');               // and unpin
  await wait(() => document.querySelector('#leases tr:nth-child(1) button').textContent === 'Закріпити');

  await p.fill('#resMac', 'de:ad:be:ef:00:01'); await p.fill('#resIp', '10.0.0.5'); await p.click('#resAdd');
  await wait(() => document.getElementById('dhcpMsg').textContent.includes('поза мережею'));
  await p.fill('#resIp', '192.168.77.50'); await p.click('#resAdd');
  await wait(() => document.querySelectorAll('#leases tr').length === 3);
  assert((await p.textContent('#leases tr:nth-child(3)')).includes('DE:AD:BE:EF:00:01'));

  assert.deepStrictEqual(errors, [], 'no page errors');
  console.log('dhcp section: list / pin / unpin / manual / errors ok');
  await b.close();
})().catch(e => { console.error(e); process.exit(1); });
