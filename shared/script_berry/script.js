/* Script section for the setup portal pages: editor, run/stop, autostart,
 * outputs, console, memory. Renders into <section id="script"> if present.
 * Load after auth.js (uses the wrapped fetch and I18N; texts: script.* in
 * i18n/<code>.json). Uses the page's CSS classes. */
(() => {
  const t = (k, p) => I18N.t(k, p);
  function el(tag, props, ...kids) {
    const e = Object.assign(document.createElement(tag), props || {});
    e.append(...kids);
    return e;
  }
  function tx(tag, key, props, ...kids) {
    const e = el(tag, props, ...kids);
    const s = el('span');
    s.dataset.i18n = key;
    s.textContent = t(key);
    e.append(s);
    return e;
  }
  const STATES = { running: 'script.running', stopped: 'script.stopped', error: 'script.errorState' };
  const kb = n => t('script.kb', { n: (n / 1024).toFixed(1) });

  function render(sec) {
    const state = el('div');
    const mem = el('div', { className: 'mute' });
    const editor = el('textarea', { spellcheck: false, rows: 14,
      style: 'width:100%;font:13px/1.4 ui-monospace,Menlo,Consolas,monospace;padding:10px;border-radius:8px;' +
             'border:1px solid var(--line);background:var(--bg);color:var(--fg);resize:vertical;tab-size:2' });
    const note = el('div', { className: 'mute' });
    const save = tx('button', 'script.save', { type: 'button' });
    const run = tx('button', 'script.run', { type: 'button', className: 'sec' });
    const stop = tx('button', 'script.stop', { type: 'button', className: 'sec' });
    const auto = el('input', { type: 'checkbox', style: 'width:auto;margin-right:8px' });
    const autoLbl = tx('label', 'script.autostart', { style: 'display:flex;align-items:center;color:var(--fg);margin-top:16px' }, auto);
    const outs = el('table');
    const consoleBox = el('pre', { style: 'max-height:220px;overflow:auto;font:12px/1.4 ui-monospace,Menlo,Consolas,monospace;' +
      'background:var(--bg);border:1px solid var(--line);border-radius:8px;padding:8px;margin:8px 0 0;white-space:pre-wrap' });
    const msg = el('div', { style: 'min-height:1.4em;margin-top:12px' });
    const say = (t, bad) => { msg.textContent = t; msg.className = bad ? 'bad' : 'ok'; };

    sec.append(tx('h2', 'script.title'), state, mem,
      tx('label', 'script.text'), editor, note,
      save, el('div', { className: 'row', style: 'display:flex;gap:8px' }, run, stop), autoLbl,
      tx('h2', 'script.results', { style: 'margin-top:16px' }), outs,
      tx('h2', 'script.console', { style: 'margin-top:16px' }), consoleBox, msg);
    let last = null, exampleShown = false;

    let since = 0, maxLen = 32768;

    fetch('/api/script', { cache: 'no-store' }).then(async r => {
      editor.value = await r.text();
      exampleShown = r.headers.get('X-Script-Saved') !== '1';
      note.textContent = exampleShown ? t('script.example') : '';
    }).catch(() => {});

    async function post(url, body, type) {
      const r = await fetch(url, { method: 'POST', body, headers: type ? { 'Content-Type': type } : {} });
      const j = await r.json().catch(() => ({ ok: false, message: t('common.noAnswer') }));
      if (!j.ok) throw new Error(I18N.msg(j));
    }

    async function poll() {
      try {
        const r = await fetch('/api/script/state?since=' + since, { cache: 'no-store' });
        const s = await r.json();
        last = s;
        show(s);
        if (s.console.length) {
          const atBottom = consoleBox.scrollTop + consoleBox.clientHeight >= consoleBox.scrollHeight - 4;
          consoleBox.textContent += s.console.join('\n') + '\n';
          if (consoleBox.textContent.length > 20000) consoleBox.textContent = consoleBox.textContent.slice(-15000);
          if (atBottom) consoleBox.scrollTop = consoleBox.scrollHeight;
        }
        since = s.seq;
      } catch (e) {}
    }

    function show(s) {
      state.textContent = t('script.state', { s: STATES[s.state] ? t(STATES[s.state]) : s.state }) + (s.error ? ' — ' + s.error : '');
      state.className = s.state === 'error' ? 'bad' : (s.state === 'running' ? 'ok' : '');
      if (s.crashDisabled) state.textContent += t('script.crashOff');
      mem.textContent = t('script.mem', { used: kb(s.mem.used), peak: kb(s.mem.peak), limit: kb(s.mem.limit), heap: kb(s.heap) });
      auto.checked = s.autostart;
      maxLen = s.maxLen;
      outs.textContent = '';
      if (!s.outputs.length) outs.append(el('tr', {}, el('td', { className: 'mute', textContent: t('script.noOutputs') })));
      for (const [k, v] of s.outputs) outs.append(el('tr', {}, el('td', { textContent: k }), el('td', { textContent: v })));
    }
    I18N.onChange(() => {
      if (last) show(last);
      if (exampleShown) note.textContent = t('script.example');
    });

    save.onclick = async () => {
      const text = editor.value;
      if (new TextEncoder().encode(text).length > maxLen) return say(t('script.tooBig', { max: kb(maxLen) }), true);
      save.disabled = true;
      try { await post('/api/script?run=1', text, 'text/plain; charset=utf-8'); exampleShown = false; note.textContent = ''; say(t('script.saved')); }
      catch (e) { say(e.message, true); }
      save.disabled = false;
      poll();
    };
    run.onclick = async () => { try { await post('/api/script/run'); say(t('script.started')); } catch (e) { say(e.message, true); } poll(); };
    stop.onclick = async () => { try { await post('/api/script/stop'); say(t('script.halted')); } catch (e) { say(e.message, true); } poll(); };
    auto.onchange = async () => {
      try { await post('/api/script/autostart', new URLSearchParams({ on: auto.checked ? '1' : '0' })); say(t(auto.checked ? 'script.autoOn' : 'script.autoOff')); }
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
    if (sec) I18N.ready.then(() => render(sec));
  });
})();
