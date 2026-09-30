// Browser test of the script section (script.js) against mock_wt32.py:
// example in the editor, save & run (state, outputs, console), stop,
// autostart, an error shown with its message, and the size check.
// Usage: node script_flow.js <port>
const { chromium } = require('playwright');
const assert = require('assert');
(async () => {
  const base = `http://127.0.0.1:${process.argv[2]}`;
  const b = await chromium.launch(process.env.CHROMIUM ? { executablePath: process.env.CHROMIUM } : {});
  const p = await b.newPage();
  const errors = []; p.on('pageerror', e => errors.push(e.message));
  const text = sel => p.textContent(sel);
  const waitText = (fn, arg) => p.waitForFunction(fn, arg, { timeout: 10000 });

  await p.goto(base);
  await p.fill('#loginOv input', '12345678'); await p.click('#loginOv button[type=submit]');
  await p.waitForSelector('#loginOv', { state: 'detached' });

  const ta = '#script textarea';
  await waitText(sel => document.querySelector(sel).value.includes('Example'), ta);
  assert((await text('#script')).includes('the editor shows an example'));

  // save & run
  await p.fill(ta, "print('hi')\noutput('Mode', 'client')\n");
  await p.click('#script button:has-text("Save and run")');
  await waitText(() => document.querySelector('#script > div').textContent.includes('running'));
  await waitText(() => document.querySelector('#script table').textContent.includes('192.168.1.50'));
  await waitText(() => document.querySelector('#script pre').textContent.includes('script started'));
  assert(!(await text('#script')).includes('the editor shows an example'));
  assert((await text('#script')).includes('Script memory: 8.8 KB'));

  // stop; console gets appended, not replaced
  await p.click('#script button:has-text("Stop")');
  await waitText(() => document.querySelector('#script > div').textContent.includes('stopped'));
  await waitText(() => document.querySelector('#script pre').textContent.includes('-- stopped'));
  assert((await text('#script pre')).includes('script started'));

  // autostart
  await p.check('#script input[type=checkbox]');
  await waitText(() => document.querySelector('#script').textContent.includes('Will run at startup'));

  // an error is shown with its message
  await p.fill(ta, 'while true end');
  await p.click('#script button:has-text("Save and run")');
  await waitText(() => document.querySelector('#script > div').textContent.includes('timeout_error'));

  // too long for the device: refused on the page, nothing sent
  await p.fill(ta, 'x'.repeat(40000));
  await p.click('#script button:has-text("Save and run")');
  await waitText(() => document.querySelector('#script').textContent.includes('The script is larger than'));

  assert.deepStrictEqual(errors, [], 'no page errors');
  console.log('script section: example / run / stop / autostart / error / size ok');
  await b.close();
})().catch(e => { console.error(e); process.exit(1); });
