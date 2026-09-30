/* Firmware update section for the setup portal pages.
 * Renders into <section id="ota"> if the page has one. Load after auth.js
 * (uses the wrapped fetch for status polling; the upload itself is an XHR
 * for progress). Uses the page's CSS classes. */
(() => {
  function el(tag, props, ...kids) {
    const e = Object.assign(document.createElement(tag), props || {});
    e.append(...kids);
    return e;
  }
  const sleep = ms => new Promise(r => setTimeout(r, ms));

  async function status() {
    const r = await fetch('/api/status', { cache: 'no-store' });
    if (!r.ok) throw new Error('status ' + r.status);
    return r.json();
  }

  function render(sec) {
    const info = el('div', { className: 'mute' });
    const file = el('input', { type: 'file', accept: '.bin' });
    const bar = el('progress', { max: 100, value: 0, hidden: true, style: 'width:100%;margin-top:12px' });
    const btn = el('button', { type: 'button', textContent: 'Оновити прошивку' });
    const msg = el('div', { style: 'min-height:1.4em;margin-top:12px' });
    const say = (t, cls) => { msg.textContent = t; msg.className = cls || ''; };
    let before = null;

    sec.append(el('h2', { textContent: 'Оновлення прошивки' }), info,
      el('label', { textContent: 'Файл «…-ota.bin»' }), file, bar, btn, msg);

    status().then(s => {
      before = s;
      if (!s.ota || !s.ota.supported) {
        info.textContent = 'Ця прошивка не підтримує оновлення через сторінку — прошийте кабелем.';
        btn.disabled = file.disabled = true;
        return;
      }
      info.textContent = `Зараз: версія ${s.version}, розділ ${s.ota.running}` +
        (s.ota.probation ? ' (нова прошивка на перевірці — не вимикайте хвилину)' : '');
    }).catch(() => {});

    btn.onclick = () => {
      const f = file.files[0];
      if (!f) return say('Оберіть файл', 'bad');
      if (before && before.ota && f.size > before.ota.maxSize) return say('Файл завеликий для розділу', 'bad');
      btn.disabled = file.disabled = true;
      bar.hidden = false; bar.value = 0;
      say('Завантаження…');
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/ota');
      xhr.setRequestHeader('Content-Type', 'application/octet-stream');
      xhr.upload.onprogress = e => { if (e.lengthComputable) bar.value = Math.round(e.loaded * 100 / e.total); };
      xhr.onload = async () => {
        let j = {};
        try { j = JSON.parse(xhr.responseText); } catch (e) {}
        if (xhr.status === 401) { say('Сесія закінчилась — оновіть сторінку й увійдіть знову', 'bad'); btn.disabled = file.disabled = false; return; }
        if (!j.ok) { say(j.message || 'Помилка оновлення', 'bad'); btn.disabled = file.disabled = false; bar.hidden = true; return; }
        say(`Записано версію ${j.version}. Перезапуск…`);
        await waitBack(j.version, before && before.ota.running);
      };
      xhr.onerror = () => { say('Звʼязок перервано під час завантаження', 'bad'); btn.disabled = file.disabled = false; };
      xhr.send(f);
    };

    async function waitBack(expected, prevPart) {
      await sleep(4000);
      for (let i = 0; i < 45; i++) {
        try {
          const s = await status();
          if (s.ota.running !== prevPart) {       /* booted from the other slot */
            say(`Оновлено до версії ${s.version}. Вона стане постійною через хвилину роботи.`, 'ok');
            before = s;
          } else {
            say(`WT32 працює з версією ${s.version}: нова не запустилась, повернуто попередню.`, 'bad');
          }
          info.textContent = `Зараз: версія ${s.version}, розділ ${s.ota.running}`;
          btn.disabled = file.disabled = false; bar.hidden = true;
          return;
        } catch (e) { await sleep(2000); }
      }
      say('Пристрій не відповідає. Перевірте, чи змінилась його адреса, і відкрийте сторінку знову.', 'bad');
    }
  }

  document.addEventListener('DOMContentLoaded', () => {
    const sec = document.getElementById('ota');
    if (sec) render(sec);
  });
})();
