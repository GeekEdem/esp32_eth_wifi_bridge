/* Script section for the setup portal pages: editor, run/stop, autostart,
 * outputs, console, memory. Renders into <section id="script"> if present.
 * Load after auth.js (uses the wrapped fetch). Uses the page's CSS classes. */
(() => {
  function el(tag, props, ...kids) {
    const e = Object.assign(document.createElement(tag), props || {});
    e.append(...kids);
    return e;
  }
  const STATES = { running: 'працює', stopped: 'зупинено', error: 'помилка' };
  const kb = n => (n / 1024).toFixed(1) + ' КБ';

  function render(sec) {
    const state = el('div');
    const mem = el('div', { className: 'mute' });
    const editor = el('textarea', { spellcheck: false, rows: 14,
      style: 'width:100%;font:13px/1.4 ui-monospace,Menlo,Consolas,monospace;padding:10px;border-radius:8px;' +
             'border:1px solid var(--line);background:var(--bg);color:var(--fg);resize:vertical;tab-size:2' });
    const note = el('div', { className: 'mute' });
    const save = el('button', { type: 'button', textContent: 'Зберегти й запустити' });
    const run = el('button', { type: 'button', className: 'sec', textContent: 'Запустити' });
    const stop = el('button', { type: 'button', className: 'sec', textContent: 'Зупинити' });
    const auto = el('input', { type: 'checkbox', style: 'width:auto;margin-right:8px' });
    const autoLbl = el('label', { style: 'display:flex;align-items:center;color:var(--fg);margin-top:16px' }, auto, 'Запускати при старті');
    const outs = el('table');
    const consoleBox = el('pre', { style: 'max-height:220px;overflow:auto;font:12px/1.4 ui-monospace,Menlo,Consolas,monospace;' +
      'background:var(--bg);border:1px solid var(--line);border-radius:8px;padding:8px;margin:8px 0 0;white-space:pre-wrap' });
    const msg = el('div', { style: 'min-height:1.4em;margin-top:12px' });
    const say = (t, bad) => { msg.textContent = t; msg.className = bad ? 'bad' : 'ok'; };

    sec.append(el('h2', { textContent: 'Скрипт (Berry)' }), state, mem,
      el('label', { textContent: 'Текст скрипта' }), editor, note,
      save, el('div', { className: 'row', style: 'display:flex;gap:8px' }, run, stop), autoLbl,
      el('h2', { textContent: 'Результати', style: 'margin-top:16px' }), outs,
      el('h2', { textContent: 'Консоль', style: 'margin-top:16px' }), consoleBox, msg);

    let since = 0, maxLen = 32768;

    fetch('/api/script', { cache: 'no-store' }).then(async r => {
      editor.value = await r.text();
      note.textContent = r.headers.get('X-Script-Saved') === '1' ? '' : 'Скрипт ще не збережено — у редакторі приклад.';
    }).catch(() => {});

    async function post(url, body, type) {
      const r = await fetch(url, { method: 'POST', body, headers: type ? { 'Content-Type': type } : {} });
      const j = await r.json().catch(() => ({ ok: false, message: 'Немає відповіді' }));
      if (!j.ok) throw new Error(j.message || 'Помилка');
    }

    async function poll() {
      try {
        const r = await fetch('/api/script/state?since=' + since, { cache: 'no-store' });
        const s = await r.json();
        state.textContent = 'Стан: ' + (STATES[s.state] || s.state) + (s.error ? ' — ' + s.error : '');
        state.className = s.state === 'error' ? 'bad' : (s.state === 'running' ? 'ok' : '');
        if (s.crashDisabled) state.textContent += ' (автозапуск вимкнено після аварії)';
        mem.textContent = `Памʼять скрипта: ${kb(s.mem.used)} (пік ${kb(s.mem.peak)}) з ${kb(s.mem.limit)} · вільно на пристрої ${kb(s.heap)}`;
        auto.checked = s.autostart;
        maxLen = s.maxLen;
        outs.textContent = '';
        if (!s.outputs.length) outs.append(el('tr', {}, el('td', { className: 'mute', textContent: 'скрипт нічого не вивів через output()' })));
        for (const [k, v] of s.outputs) outs.append(el('tr', {}, el('td', { textContent: k }), el('td', { textContent: v })));
        if (s.console.length) {
          const atBottom = consoleBox.scrollTop + consoleBox.clientHeight >= consoleBox.scrollHeight - 4;
          consoleBox.textContent += s.console.join('\n') + '\n';
          if (consoleBox.textContent.length > 20000) consoleBox.textContent = consoleBox.textContent.slice(-15000);
          if (atBottom) consoleBox.scrollTop = consoleBox.scrollHeight;
        }
        since = s.seq;
      } catch (e) {}
    }

    save.onclick = async () => {
      const text = editor.value;
      if (new TextEncoder().encode(text).length > maxLen) return say(`Скрипт більший за ${kb(maxLen)}`, true);
      save.disabled = true;
      try { await post('/api/script?run=1', text, 'text/plain; charset=utf-8'); note.textContent = ''; say('Збережено й запущено'); }
      catch (e) { say(e.message, true); }
      save.disabled = false;
      poll();
    };
    run.onclick = async () => { try { await post('/api/script/run'); say('Запущено'); } catch (e) { say(e.message, true); } poll(); };
    stop.onclick = async () => { try { await post('/api/script/stop'); say('Зупинено'); } catch (e) { say(e.message, true); } poll(); };
    auto.onchange = async () => {
      try { await post('/api/script/autostart', new URLSearchParams({ on: auto.checked ? '1' : '0' })); say(auto.checked ? 'Запускатиметься при старті' : 'Автозапуск вимкнено'); }
      catch (e) { say(e.message, true); auto.checked = !auto.checked; }
    };
    editor.addEventListener('keydown', e => {
      if (e.key === 'Tab') { e.preventDefault(); editor.setRangeText('  ', editor.selectionStart, editor.selectionEnd, 'end'); }
    });

    poll();
    setInterval(poll, 2000);
  }

  document.addEventListener('DOMContentLoaded', () => {
    const sec = document.getElementById('script');
    if (sec) render(sec);
  });
})();
