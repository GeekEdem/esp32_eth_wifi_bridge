/* Login and password change for the setup portal pages.
 * Wraps fetch(): any 401 opens the login form once and retries the request.
 * Renders the password section into <section id="auth"> if the page has one.
 * Uses the page's CSS variables and classes (button.sec, .bad, .ok, .mute). */
(() => {
  const F = window.fetch.bind(window);
  let pending = null;

  const style = document.createElement('style');
  style.textContent = '#loginOv{position:fixed;inset:0;background:rgba(0,0,0,.55);display:flex;align-items:center;' +
    'justify-content:center;padding:16px;z-index:10}#loginOv form{background:var(--card,#fff);color:var(--fg,#111);' +
    'border:1px solid var(--line,#ccc);border-radius:12px;padding:20px;width:100%;max-width:360px}' +
    '#loginOv h2{margin:0 0 12px;font-size:18px}';
  document.head.append(style);

  function el(tag, props, ...kids) {
    const e = Object.assign(document.createElement(tag), props || {});
    e.append(...kids);
    return e;
  }

  async function postForm(url, data) {
    const r = await F(url, { method: 'POST', body: new URLSearchParams(data) });
    return r.json().catch(() => ({ ok: false, message: 'Немає відповіді' }));
  }

  function showLogin() {
    if (pending) return pending;
    pending = new Promise(resolve => {
      const pw = el('input', { type: 'password', autocomplete: 'current-password', required: true });
      const msg = el('div', { className: 'bad', style: 'min-height:1.4em;margin-top:8px' });
      const btn = el('button', { type: 'submit', textContent: 'Увійти' });
      const form = el('form', {},
        el('h2', { textContent: 'Вхід' }),
        el('label', { textContent: 'Пароль сторінки' }), pw, btn, msg,
        el('div', { className: 'mute', style: 'margin-top:8px',
          textContent: 'Якщо пароль не змінювали — 12345678.' }));
      const ov = el('div', { id: 'loginOv' }, form);
      form.onsubmit = async ev => {
        ev.preventDefault();
        btn.disabled = true;
        const j = await postForm('/api/login', { password: pw.value });
        btn.disabled = false;
        if (!j.ok) { msg.textContent = j.message || 'Помилка'; pw.select(); return; }
        ov.remove();
        pending = null;
        markDefault(j.defaultPassword);
        resolve();
      };
      document.body.append(ov);
      pw.focus();
    });
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
    warn = el('div', { className: 'bad', hidden: true,
      textContent: 'Використовується стандартний пароль 12345678 — змініть його.' });
    const oldPw = el('input', { type: 'password', autocomplete: 'current-password' });
    const newPw = el('input', { type: 'password', autocomplete: 'new-password', minLength: 8, maxLength: 63 });
    const newPw2 = el('input', { type: 'password', autocomplete: 'new-password' });
    const msg = el('div', { style: 'min-height:1.4em;margin-top:12px' });
    const say = (t, bad) => { msg.textContent = t; msg.className = bad ? 'bad' : 'ok'; };
    const change = el('button', { type: 'button', textContent: 'Змінити пароль' });
    const logout = el('button', { type: 'button', className: 'sec', textContent: 'Вийти' });
    change.onclick = async () => {
      if (newPw.value !== newPw2.value) return say('Нові паролі не збігаються', true);
      if (newPw.value.length < 8) return say('Новий пароль має бути щонайменше 8 символів', true);
      change.disabled = true;
      const r = await fetch('/api/password', { method: 'POST',
        body: new URLSearchParams({ old: oldPw.value, new: newPw.value }) });
      const j = await r.json().catch(() => ({ ok: false, message: 'Немає відповіді' }));
      change.disabled = false;
      if (!j.ok) return say(j.message || 'Помилка', true);
      oldPw.value = newPw.value = newPw2.value = '';
      markDefault(false);
      say('Пароль змінено');
    };
    logout.onclick = async () => { await postForm('/api/logout', {}); location.reload(); };
    sec.append(
      el('h2', { textContent: 'Пароль сторінки' }), warn,
      el('label', { textContent: 'Поточний пароль' }), oldPw,
      el('label', { textContent: 'Новий пароль (8–63 символи)' }), newPw,
      el('label', { textContent: 'Новий пароль ще раз' }), newPw2,
      change, logout, msg);
  }

  document.addEventListener('DOMContentLoaded', () => {
    renderSection();
    fetch('/api/status').then(r => r.json()).then(s => markDefault(s.defaultPassword)).catch(() => {});
  });
})();
