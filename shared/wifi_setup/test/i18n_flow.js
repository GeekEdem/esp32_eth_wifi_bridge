// Browser test of the page languages (auth.js I18N) against mock_portal.py or
// wt32/test/mock_wt32.py: the browser's language while the device has none,
// the first login saving it on the device, the device's language for a new
// browser, the switch on the login form (sent to the device after the login),
// a device error in the chosen language, the whole page translated (no English
// text left in Ukrainian, no Cyrillic in English), the choice kept after a
// reload, and the firmware info. Needs a fresh mock.
// Usage: node i18n_flow.js <port> <name>
const { chromium } = require('playwright');
const assert = require('assert');
(async () => {
  const [port, name] = process.argv.slice(2);
  const base = `http://127.0.0.1:${port}`;
  const b = await chromium.launch(process.env.CHROMIUM ? { executablePath: process.env.CHROMIUM } : {});
  const errors = [];
  const newPage = async locale => {
    const ctx = await b.newContext({ locale });
    const p = await ctx.newPage();
    p.on('pageerror', e => errors.push(e.message));
    return p;
  };
  const texts = async code => (await (await fetch(`${base}/i18n.json?l=${code}`)).json());
  const en = await texts('en'), uk = await texts('uk');
  assert.strictEqual(en.lang, 'en'); assert.strictEqual(uk.lang, 'uk');
  assert.deepStrictEqual(en.langs.map(l => l.code), ['en', 'uk']);
  assert.strictEqual((await texts('xx')).lang, 'en', 'unknown language: the first one');
  // English texts that the Ukrainian file really translates (not "Wi-Fi", "Ethernet", ...)
  const englishOnly = Object.keys(uk.strings).filter(k => uk.strings[k] !== en.strings[k] && !/\{\w+\}/.test(en.strings[k]))
    .map(k => en.strings[k].trim()).filter(s => s.length > 2);

  // Everything the user can read: text nodes, placeholders, titles.
  const readable = p => p.evaluate(() => {
    const out = [];
    const w = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
    for (let n; (n = w.nextNode());) {
      const par = n.parentElement;
      if (par && !['SCRIPT', 'STYLE', 'TEXTAREA', 'PRE', 'CODE'].includes(par.tagName) && n.textContent.trim()) out.push(n.textContent.trim());
    }
    document.querySelectorAll('[placeholder]').forEach(e => out.push(e.placeholder));
    document.querySelectorAll('[title]').forEach(e => out.push(e.title));
    out.push(document.title);
    return out;
  });

  const saved = async () => (await (await fetch(`${base}/_lang`)).json()).saved;
  const overlayIn = (p, texts) => p.waitForFunction(t => document.querySelector('#loginOv h2')?.textContent === t,
    texts.strings['auth.title'], { timeout: 5000 });
  const login = async (p, pw) => {
    await p.fill('#loginOv input', pw); await p.click('#loginOv button[type=submit]');
  };

  // 0. nothing saved on the device: a Ukrainian browser gets Ukrainian; its
  //    first login saves that as the device's language (the display uses it)
  assert.strictEqual(await saved(), null);
  let p = await newPage('uk-UA');
  await p.goto(base);
  await overlayIn(p, uk);
  await login(p, '12345678');
  await p.waitForSelector('#loginOv', { state: 'detached' });
  await p.waitForFunction(() => document.documentElement.lang === 'uk');
  for (let i = 0; i < 20 && await saved() !== 'uk'; i++) await p.waitForTimeout(100);
  assert.strictEqual(await saved(), 'uk', 'first login saves the language');
  await p.context().close();

  // 1. an English browser without a choice: the device's language (Ukrainian);
  //    switched to English on the login form, a wrong password in English,
  //    sent to the device after the login
  p = await newPage('en-US');
  await p.goto(base);
  await overlayIn(p, uk);
  assert.deepStrictEqual(await p.$$eval('#loginOv .langsw button', bs => bs.map(x => x.textContent)), ['EN', 'УКР']);
  await p.click('#loginOv .langsw button[data-lang=en]');
  await overlayIn(p, en);
  assert.strictEqual(await p.textContent('#loginOv button[type=submit]'), en.strings['auth.login']);
  await login(p, 'wrong');
  await p.waitForFunction(t => document.querySelector('#loginOv .bad').textContent === t, en.strings['err.wrongPassword']);
  assert.strictEqual(await saved(), 'uk', 'not logged in yet: the device keeps its language');
  await login(p, '12345678');
  await p.waitForSelector('#loginOv', { state: 'detached' });
  for (let i = 0; i < 20 && await saved() !== 'en'; i++) await p.waitForTimeout(100);
  assert.strictEqual(await saved(), 'en', 'the choice made before login reaches the device');

  // 2. the page is in English, with the firmware info
  await p.waitForFunction(() => document.querySelector('#ota table code'), null, { timeout: 5000 });
  await p.waitForTimeout(300);                       // status-driven parts
  assert.strictEqual(await p.evaluate(() => document.documentElement.lang), 'en');
  const own = en.langs.flatMap(l => [l.name, l.label]);         // each language is named in itself
  const cyr = (await readable(p)).filter(s => /[Ѐ-ӿ]/.test(s) && !own.includes(s));
  assert.deepStrictEqual(cyr, [], 'Cyrillic left in English');
  const ota = await p.textContent('#ota');
  for (const s of [en.strings['fw.commit'], en.strings['fw.elf'], '0123456789', '89abcdef01234567', 'Sep 30 2026 12:00:00'])
    assert(ota.includes(s), `firmware info: ${s}`);

  // 3. the choice survives a reload; the header switch goes to Ukrainian, on
  //    the page and on the device at once
  await p.reload();
  await p.waitForSelector('h1 .langsw button.on[data-lang=en]', { timeout: 5000 });
  await p.click('h1 .langsw button[data-lang=uk]');
  await p.waitForFunction(() => document.documentElement.lang === 'uk');
  await p.waitForTimeout(300);
  const left = (await readable(p)).filter(s => englishOnly.includes(s));
  assert.deepStrictEqual(left, [], 'English left in Ukrainian');
  assert((await p.textContent('#ota')).includes(uk.strings['fw.commit']));
  for (let i = 0; i < 20 && await saved() !== 'uk'; i++) await p.waitForTimeout(100);
  assert.strictEqual(await saved(), 'uk');
  await p.context().close();

  // 4. setting the device's language needs a session
  const r = await fetch(`${base}/api/lang`, { method: 'POST', body: new URLSearchParams({ lang: 'xx' }) });
  assert.strictEqual(r.status, 401, 'needs a session');

  assert.deepStrictEqual(errors, [], 'no page errors');
  console.log(`${name}: languages ok`);
  await b.close();
})().catch(e => { console.error(e); process.exit(1); });
