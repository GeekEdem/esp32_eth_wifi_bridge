// Browser test of the page languages (auth.js I18N) against mock_portal.py or
// wt32/test/mock_wt32.py: the switch on the login form, a device error in the
// chosen language, the whole page translated (no English text left in
// Ukrainian, no Cyrillic in English), the choice kept after a reload, the
// browser's language as the default, and the firmware info.
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

  // 1. English browser: English login form with the switch; switch there to Ukrainian
  let p = await newPage('en-US');
  await p.goto(base);
  await p.waitForSelector('#loginOv .langsw button', { timeout: 5000 });
  assert.strictEqual(await p.textContent('#loginOv h2'), en.strings['auth.title']);
  assert.deepStrictEqual(await p.$$eval('#loginOv .langsw button', bs => bs.map(x => x.textContent)), ['EN', 'УКР']);
  await p.click('#loginOv .langsw button[data-lang=uk]');
  await p.waitForFunction(t => document.querySelector('#loginOv h2').textContent === t, uk.strings['auth.title']);
  assert.strictEqual(await p.textContent('#loginOv button[type=submit]'), uk.strings['auth.login']);
  await p.fill('#loginOv input', 'wrong'); await p.click('#loginOv button[type=submit]');
  await p.waitForFunction(t => document.querySelector('#loginOv .bad').textContent === t, uk.strings['err.wrongPassword']);
  await p.fill('#loginOv input', '12345678'); await p.click('#loginOv button[type=submit]');
  await p.waitForSelector('#loginOv', { state: 'detached' });

  // 2. the page is in Ukrainian, with the firmware info
  await p.waitForFunction(() => document.querySelector('#ota table code'), null, { timeout: 5000 });
  await p.waitForTimeout(300);                       // status-driven parts
  assert.strictEqual(await p.evaluate(() => document.documentElement.lang), 'uk');
  let left = (await readable(p)).filter(s => englishOnly.includes(s));
  assert.deepStrictEqual(left, [], 'English left in Ukrainian');
  const ota = await p.textContent('#ota');
  for (const s of [uk.strings['fw.commit'], uk.strings['fw.elf'], '0123456789', '89abcdef01234567', 'Sep 30 2026 12:00:00'])
    assert(ota.includes(s), `firmware info: ${s}`);

  // 3. the choice survives a reload; the header switch goes back to English
  await p.reload();
  await p.waitForSelector('h1 .langsw button.on[data-lang=uk]', { timeout: 5000 });
  await p.click('h1 .langsw button[data-lang=en]');
  await p.waitForFunction(() => document.documentElement.lang === 'en');
  await p.waitForTimeout(300);
  const own = en.langs.flatMap(l => [l.name, l.label]);         // each language is named in itself
  const cyr = (await readable(p)).filter(s => /[Ѐ-ӿ]/.test(s) && !own.includes(s));
  assert.deepStrictEqual(cyr, [], 'Cyrillic left in English');
  assert((await p.textContent('#ota')).includes(en.strings['fw.commit']));
  await p.context().close();

  // 4. a Ukrainian browser starts in Ukrainian
  p = await newPage('uk-UA');
  await p.goto(base);
  await p.waitForFunction(t => document.querySelector('#loginOv h2')?.textContent === t, uk.strings['auth.title'], { timeout: 5000 });
  await p.context().close();

  assert.deepStrictEqual(errors, [], 'no page errors');
  console.log(`${name}: languages ok`);
  await b.close();
})().catch(e => { console.error(e); process.exit(1); });
