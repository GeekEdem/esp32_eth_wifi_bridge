/* Language, login and password change for the setup portal pages.
 *
 * Language: the texts come from /i18n.json?l=<code> (built from the
 * i18n/<code>.json files, see tools/i18n_bundle.py), which also lists the
 * languages. I18N.t(key, {param}) gives the text in the current language;
 * elements with data-i18n / data-i18n-ph / data-i18n-title get their text /
 * placeholder / title from it (the HTML keeps the English text, shown until the
 * texts are loaded); I18N.onChange(fn) lets a page redraw what it builds
 * itself; I18N.ready resolves once the first texts are in. The choice (switch
 * in the page header and on the login form) is kept in localStorage and on the
 * device (POST /api/lang: its display uses it; a switch before login is sent
 * after it, and the first login sets it if the device has none yet). Without a
 * choice in this browser: the device's language, else the browser's if the
 * device has it, else the first language.
 * Errors from the device carry a key ({"key":"err.…","message":"English"}):
 * I18N.msg(j) gives the text in the current language.
 *
 * Login: wraps fetch(); any 401 opens the login form once and retries the
 * request. Renders the password section into <section id="auth"> if the page
 * has one. Uses the page's CSS variables and classes (button.sec, .bad, .ok, .mute). */
(() => {
  let dict = {};
  let langs = [];
  let lang = (() => {
    try { return localStorage.getItem('lang') || ''; } catch (e) { return ''; }
  })();
  let deviceSaved = true;        /* the device has a language saved */
  let pendingLang = null;        /* chosen before login: send to the device after it */
  const listeners = [];

  function t(key, p) {
    let s = dict[key];
    if (s === undefined) s = key;
    return p ? s.replace(/\{(\w+)\}/g, (m, k) => (k in p ? p[k] : m)) : s;
  }
  function msg(j, fallbackKey) {
    if (j && j.key && dict[j.key] !== undefined) return t(j.key);
    if (j && j.message) return j.message;
    return t(fallbackKey || 'common.error');
  }
  function fillSwitch(box) {
    box.textContent = '';
    if (langs.length < 2) { box.hidden = true; return; }
    box.hidden = false;
    if (langs.length > 3) {
      const sel = document.createElement('select');
      sel.title = t('common.language');
      for (const l of langs) sel.append(Object.assign(document.createElement('option'), { value: l.code, textContent: l.name }));
      sel.value = lang;
      sel.onchange = () => set(sel.value);
      box.append(sel);
      return;
    }
    for (const l of langs) {
      const b = document.createElement('button');
      b.type = 'button';
      b.dataset.lang = l.code;
      b.textContent = l.label;
      b.title = l.name;
      b.className = l.code === lang ? 'on' : '';
      b.onclick = () => set(l.code);
      box.append(b);
    }
  }
  function apply(root) {
    root = root || document;
    const has = k => dict[k] !== undefined;
    root.querySelectorAll('[data-i18n]').forEach(e => { if (has(e.dataset.i18n)) e.textContent = t(e.dataset.i18n); });
    root.querySelectorAll('[data-i18n-ph]').forEach(e => { if (has(e.dataset.i18nPh)) e.placeholder = t(e.dataset.i18nPh); });
    root.querySelectorAll('[data-i18n-title]').forEach(e => { if (has(e.dataset.i18nTitle)) e.title = t(e.dataset.i18nTitle); });
    document.documentElement.lang = lang;
    const title = document.querySelector('title[data-i18n]');
    if (title && has(title.dataset.i18n)) document.title = t(title.dataset.i18n);
    document.querySelectorAll('.langsw').forEach(fillSwitch);
  }
  /* code '': the device decides (see the top), told the browser's language */
  async function load(code) {
    const q = code ? 'l=' + encodeURIComponent(code)
      : 'b=' + encodeURIComponent((navigator.language || 'en').slice(0, 2).toLowerCase());
    const r = await window.fetch('/i18n.json?' + q, { cache: 'no-store' });
    const j = await r.json();
    dict = j.strings || {};
    langs = j.langs || [];
    lang = j.lang || code;
    deviceSaved = j.saved !== false;
  }
  /* Save the language on the device; before login (401) it waits for the login. */
  function saveDevice(code) {
    pendingLang = code;
    F('/api/lang', { method: 'POST', body: new URLSearchParams({ lang: code }) })
      .then(r => { if (r.status !== 401) { pendingLang = null; deviceSaved = true; } })
      .catch(() => {});
  }
  async function set(code) {
    if (code === lang) return;
    try { await load(code); } catch (e) { return; }
    try { localStorage.setItem('lang', lang); } catch (e) {}
    apply();
    listeners.forEach(f => { try { f(lang); } catch (e) { console.error(e); } });
    saveDevice(lang);
  }
  function switcher() {
    const box = document.createElement('div');
    box.className = 'langsw';
    fillSwitch(box);
    return box;
  }
  const ready = load(lang).catch(() => {}).then(() => {
    if (document.readyState !== 'loading') apply();
  });
  window.I18N = {
    t, msg, apply, set, switcher, ready,
    onChange(f) { listeners.push(f); },
    get lang() { return lang; },
  };

  const F = window.fetch.bind(window);
  let pending = null;

  const style = document.createElement('style');
  style.textContent = '#loginOv{position:fixed;inset:0;background:rgba(0,0,0,.55);display:flex;align-items:center;' +
    'justify-content:center;padding:16px;z-index:10}#loginOv form{background:var(--card,#fff);color:var(--fg,#111);' +
    'border:1px solid var(--line,#ccc);border-radius:12px;padding:20px;width:100%;max-width:360px}' +
    '#loginOv h2{margin:0;font-size:18px}#loginOv .head{display:flex;align-items:center;justify-content:space-between;margin:0 0 12px}' +
    '.langsw{display:inline-flex;gap:0;border:1px solid var(--line,#ccc);border-radius:8px;overflow:hidden;flex:none}' +
    '.langsw button{width:auto;margin:0;padding:6px 10px;border:0;border-radius:0;background:transparent;' +
    'color:var(--mute,#666);font-size:13px;font-weight:600}.langsw button.on{background:var(--acc,#2563eb);color:#fff}' +
    '.langsw select{width:auto;margin:0;padding:6px 8px;border:0;font-size:13px}' +
    'h1.withlang{display:flex;align-items:center;justify-content:space-between;gap:12px}';
  document.head.append(style);

  function el(tag, props, ...kids) {
    const e = Object.assign(document.createElement(tag), props || {});
    e.append(...kids);
    return e;
  }
  /* Element whose text follows the language. */
  function tx(tag, key, props) {
    const e = el(tag, props);
    e.dataset.i18n = key;
    e.textContent = t(key);
    return e;
  }

  async function postForm(url, data) {
    const r = await F(url, { method: 'POST', body: new URLSearchParams(data) });
    return r.json().catch(() => ({ ok: false, message: t('common.noAnswer') }));
  }

  function showLogin() {
    if (pending) return pending;
    pending = ready.then(() => new Promise(resolve => {
      const pw = el('input', { type: 'password', autocomplete: 'current-password', required: true });
      const msg = el('div', { className: 'bad', style: 'min-height:1.4em;margin-top:8px' });
      const btn = tx('button', 'auth.login', { type: 'submit' });
      const form = el('form', {},
        el('div', { className: 'head' }, tx('h2', 'auth.title'), switcher()),
        tx('label', 'auth.password'), pw, btn, msg,
        tx('div', 'auth.defaultHint', { className: 'mute', style: 'margin-top:8px' }));
      const ov = el('div', { id: 'loginOv' }, form);
      form.onsubmit = async ev => {
        ev.preventDefault();
        btn.disabled = true;
        const j = await postForm('/api/login', { password: pw.value });
        btn.disabled = false;
        if (!j.ok) { msg.textContent = I18N.msg(j); pw.select(); return; }
        ov.remove();
        pending = null;
        if (pendingLang || !deviceSaved) saveDevice(pendingLang || lang);
        markDefault(j.defaultPassword);
        resolve();
      };
      document.body.append(ov);
      apply(ov);
      pw.focus();
    }));
    return pending;
  }

  window.fetch = async (url, opts) => {
    let r = await F(url, opts);
    if (r.status === 401 && !String(url).startsWith('/api/login')) {
      await showLogin();
      r = await F(url, opts);
    }
    return r;
  };

  let warn = null;
  function markDefault(on) {
    if (warn) warn.hidden = !on;
  }

  function renderSection() {
    const sec = document.getElementById('auth');
    if (!sec) return;
    warn = tx('div', 'auth.defaultWarn', { className: 'bad', hidden: true });
    const oldPw = el('input', { type: 'password', autocomplete: 'current-password' });
    const newPw = el('input', { type: 'password', autocomplete: 'new-password', minLength: 8, maxLength: 63 });
    const newPw2 = el('input', { type: 'password', autocomplete: 'new-password' });
    const msg = el('div', { style: 'min-height:1.4em;margin-top:12px' });
    const say = (s, bad) => { msg.textContent = s; msg.className = bad ? 'bad' : 'ok'; };
    const change = tx('button', 'auth.change', { type: 'button' });
    const logout = tx('button', 'auth.logout', { type: 'button', className: 'sec' });
    change.onclick = async () => {
      if (newPw.value !== newPw2.value) return say(t('auth.mismatch'), true);
      if (newPw.value.length < 8) return say(t('auth.short'), true);
      change.disabled = true;
      const r = await fetch('/api/password', { method: 'POST',
        body: new URLSearchParams({ old: oldPw.value, new: newPw.value }) });
      const j = await r.json().catch(() => ({ ok: false, message: t('common.noAnswer') }));
      change.disabled = false;
      if (!j.ok) return say(I18N.msg(j), true);
      oldPw.value = newPw.value = newPw2.value = '';
      markDefault(false);
      say(t('auth.changed'));
    };
    logout.onclick = async () => { await postForm('/api/logout', {}); location.reload(); };
    sec.append(
      tx('h2', 'auth.section'), warn,
      tx('label', 'auth.current'), oldPw,
      tx('label', 'auth.new'), newPw,
      tx('label', 'auth.new2'), newPw2,
      change, logout, msg);
  }

  document.addEventListener('DOMContentLoaded', () => {
    const h1 = document.querySelector('h1');
    if (h1) {
      const name = el('span');
      name.append(...h1.childNodes);
      if (h1.dataset.i18n) { name.dataset.i18n = h1.dataset.i18n; delete h1.dataset.i18n; }
      h1.append(name, switcher());
      h1.classList.add('withlang');
    }
    ready.then(() => {
      renderSection();
      apply();
      fetch('/api/status').then(r => r.json()).then(s => markDefault(s.defaultPassword)).catch(() => {});
    });
  });
})();
