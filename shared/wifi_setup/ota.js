/* Firmware section for the setup portal pages: which firmware runs (version,
 * build date, source commit, SHA-256, ESP-IDF) and the update. Renders into
 * <section id="ota"> if the page has one. Load after auth.js (uses the wrapped
 * fetch and I18N; the upload itself is an XHR for progress). Texts: ota.* and
 * fw.* in the i18n files. Uses the page's CSS classes. */
(() => {
  const t = (k, p) => I18N.t(k, p);
  function el(tag, props, ...kids) {
    const e = Object.assign(document.createElement(tag), props || {});
    e.append(...kids);
    return e;
  }
  function tx(tag, key, props) {
    const e = el(tag, props);
    e.dataset.i18n = key;
    e.textContent = t(key);
    return e;
  }
  const sleep = ms => new Promise(r => setTimeout(r, ms));

  async function status() {
    const r = await fetch('/api/status', { cache: 'no-store' });
    if (!r.ok) throw new Error('status ' + r.status);
    return r.json();
  }

  function render(sec) {
    const info = el('table');
    const note = el('div', { className: 'mute', style: 'margin-top:8px' });
    const file = el('input', { type: 'file', accept: '.bin' });
    const bar = el('progress', { max: 100, value: 0, hidden: true, style: 'width:100%;margin-top:12px' });
    const btn = tx('button', 'ota.update', { type: 'button' });
    const msg = el('div', { style: 'min-height:1.4em;margin-top:12px' });
    const say = (s, cls) => { msg.textContent = s; msg.className = cls || ''; };
    let before = null;

    sec.append(tx('h2', 'ota.title'), info, note, tx('label', 'ota.file'), file, bar, btn, msg);

    function row(key, value) {
      const code = el('code', { textContent: value || '—' });
      info.append(el('tr', {}, tx('td', key), el('td', {}, code)));
    }
    function showInfo(s) {
      info.textContent = '';
      const b = s.build || {};
      row('fw.version', b.version || s.version);
      row('fw.built', [b.date, b.time].filter(Boolean).join(' '));
      row('fw.commit', b.commit);
      row('fw.elf', b.elf);
      row('fw.idf', b.idf);
      if (!s.ota || !s.ota.supported) {
        note.textContent = t('ota.unsupported');
        btn.disabled = file.disabled = true;
        return;
      }
      note.textContent = t('ota.partition', { p: s.ota.running }) + (s.ota.probation ? ' ' + t('ota.probation') : '');
    }
    status().then(s => { before = s; showInfo(s); }).catch(() => {});
    I18N.onChange(() => { if (before) showInfo(before); });

    btn.onclick = () => {
      const f = file.files[0];
      if (!f) return say(t('ota.choose'), 'bad');
      if (before && before.ota && f.size > before.ota.maxSize) return say(t('ota.tooBig'), 'bad');
      btn.disabled = file.disabled = true;
      bar.hidden = false; bar.value = 0;
      say(t('ota.uploading'));
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/ota');
      xhr.setRequestHeader('Content-Type', 'application/octet-stream');
      xhr.upload.onprogress = e => { if (e.lengthComputable) bar.value = Math.round(e.loaded * 100 / e.total); };
      xhr.onload = async () => {
        let j = {};
        try { j = JSON.parse(xhr.responseText); } catch (e) {}
        if (xhr.status === 401) { say(t('ota.session'), 'bad'); btn.disabled = file.disabled = false; return; }
        if (!j.ok) { say(j.key || j.message ? I18N.msg(j) : t('ota.failed'), 'bad'); btn.disabled = file.disabled = false; bar.hidden = true; return; }
        say(t('ota.written', { v: j.version }));
        await waitBack(j.version, before && before.ota.running);
      };
      xhr.onerror = () => { say(t('ota.lost'), 'bad'); btn.disabled = file.disabled = false; };
      xhr.send(f);
    };

    async function waitBack(expected, prevPart) {
      await sleep(4000);
      for (let i = 0; i < 45; i++) {
        try {
          const s = await status();
          if (s.ota.running !== prevPart) {       /* booted from the other slot */
            say(t('ota.done', { v: s.version }), 'ok');
          } else {
            say(t('ota.rolledBack', { v: s.version }), 'bad');
          }
          before = s;
          showInfo(s);
          btn.disabled = file.disabled = false; bar.hidden = true;
          return;
        } catch (e) { await sleep(2000); }
      }
      say(t('ota.gone'), 'bad');
    }
  }

  document.addEventListener('DOMContentLoaded', () => {
    const sec = document.getElementById('ota');
    if (sec) I18N.ready.then(() => render(sec));
  });
})();
