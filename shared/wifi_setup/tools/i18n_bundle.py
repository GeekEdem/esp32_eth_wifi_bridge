#!/usr/bin/env python3
"""Page languages: i18n/<code>.json files -> one bundle per app.

Every component with page texts keeps them in an i18n/ folder, one flat JSON
object per language: {"_name": "English", "_label": "EN", "key": "text", ...}.
"_name" and "_label" (the language's own name and the short label on the
switch) are needed once per language, in any of the folders. Texts may hold
{placeholders}; a translation must use the same ones. English (en.json) is the
reference: every key must exist there, and a language that lacks a key gets
the English text in the bundle.

To add a language, put <code>.json next to en.json in the folders (at least the
app's own one) and rebuild; the switch on the page lists it by itself.

    i18n_bundle.py c OUT.c DIR...          C source with `portal_langs` (build step)
    i18n_bundle.py json OUTDIR DIR...      OUTDIR/<code>.json merged (test mocks)
    i18n_bundle.py check [--complete] DIR... [--sources FILE...]
        validate the files; with --sources also that every key used in them
        (data-i18n*, t('...'), I18N.t('...'), error keys "err.*") exists;
        with --complete a text missing in a language is an error, not a warning
"""
import glob
import json
import os
import re
import sys

META = ('_name', '_label')
PLACEHOLDER = re.compile(r'\{(\w+)\}')


def load(dirs):
    """{code: {key: text}} merged over the folders; errors on conflicts."""
    langs, errors = {}, []
    for d in dirs:
        for path in sorted(glob.glob(os.path.join(d, '*.json'))):
            code = os.path.splitext(os.path.basename(path))[0]
            try:
                data = json.load(open(path, encoding='utf-8'))
            except ValueError as e:
                errors.append('%s: %s' % (path, e))
                continue
            if not isinstance(data, dict):
                errors.append('%s: not a JSON object' % path)
                continue
            merged = langs.setdefault(code, {})
            for k, v in data.items():
                if not isinstance(v, str) or not v:
                    errors.append('%s: "%s" must be a non-empty string' % (path, k))
                elif k in merged and merged[k] != v:
                    errors.append('%s: "%s" differs from another folder ("%s" vs "%s")' % (path, k, v, merged[k]))
                else:
                    merged[k] = v
    return langs, errors


def validate(langs):
    errors, warnings = [], []
    en = langs.get('en')
    if en is None:
        return ['no en.json'], []
    for code, texts in sorted(langs.items()):
        for m in META:
            if m not in texts:
                errors.append('%s: "%s" missing' % (code, m))
        for k, v in texts.items():
            if k in META:
                continue
            if k not in en:
                errors.append('%s: "%s" is not in en.json (typo?)' % (code, k))
            elif set(PLACEHOLDER.findall(v)) != set(PLACEHOLDER.findall(en[k])):
                errors.append('%s: "%s" placeholders %s differ from English %s'
                              % (code, k, sorted(set(PLACEHOLDER.findall(v))), sorted(set(PLACEHOLDER.findall(en[k])))))
        missing = [k for k in en if k not in texts and k not in META]
        if missing and code != 'en':
            warnings.append('%s: %d texts fall back to English: %s' % (code, len(missing), ', '.join(missing[:8])
                                                                       + (' …' if len(missing) > 8 else '')))
    return errors, warnings


def merged(langs):
    """Per language: English texts overlaid with the language's own."""
    en = langs['en']
    out = {}
    for code in ['en'] + sorted(c for c in langs if c != 'en'):
        texts = {k: v for k, v in en.items() if k not in META}
        texts.update({k: v for k, v in langs[code].items() if k not in META})
        out[code] = (langs[code]['_name'], langs[code]['_label'], texts)
    return out


def c_string(s):
    b = s.encode('utf-8')
    out, line = [], ''
    for ch in b:
        c = chr(ch)
        piece = {'"': '\\"', '\\': '\\\\', '\n': '\\n'}.get(c, c) if 32 <= ch < 127 else '\\%03o' % ch
        line += piece
        if len(line) > 100:
            out.append('"%s"' % line)
            line = ''
    out.append('"%s"' % line)
    return '\n    '.join(out)


def keys_used(paths):
    used = {}
    pats = [re.compile(r'data-i18n(?:-ph|-title)?="([\w.]+)"'),
            re.compile(r"""(?<![\w.])(?:I18N\.)?t\(\s*'([\w.]+)'"""),
            re.compile(r"""\btx\(\s*'[\w]+'\s*,\s*'([\w.]+)'"""),
            re.compile(r'"(err\.\w+)"')]
    for p in paths:
        src = open(p, encoding='utf-8').read()
        for pat in pats:
            for k in pat.findall(src):
                used.setdefault(k, p)
    return used


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    mode = argv[1]
    if mode == 'check':
        args = argv[2:]
        sources = []
        complete = '--complete' in args
        args = [a for a in args if a != '--complete']
        if '--sources' in args:
            i = args.index('--sources')
            args, sources = args[:i], args[i + 1:]
        langs, errors = load(args)
        e2, warnings = validate(langs)
        errors += e2
        if sources and 'en' in langs:
            for k, where in sorted(keys_used(sources).items()):
                if k not in langs['en']:
                    errors.append('%s uses "%s", which is not in en.json' % (where, k))
        if complete:
            errors += warnings
            warnings = []
        for w in warnings:
            print('warning:', w)
        for e in errors:
            print('error:', e)
        print('%d languages (%s), %d texts: %s' % (len(langs), ', '.join(sorted(langs)),
                                                   len(langs.get('en', {})) - len(META), 'ok' if not errors else 'FAILED'))
        return 1 if errors else 0
    out, dirs = argv[2], argv[3:]
    langs, errors = load(dirs)
    e2, _ = validate(langs)
    errors += e2
    if errors:
        for e in errors:
            print('i18n error:', e, file=sys.stderr)
        return 1
    m = merged(langs)
    if mode == 'json':
        os.makedirs(out, exist_ok=True)
        for code, (name, label, texts) in m.items():
            json.dump({'_name': name, '_label': label, **texts}, open(os.path.join(out, code + '.json'), 'w', encoding='utf-8'),
                      ensure_ascii=False, indent=0)
        return 0
    lines = ['/* Generated by shared/wifi_setup/tools/i18n_bundle.py from:', ' *   ' + '\n *   '.join(dirs),
             ' * Do not edit: change the i18n/<code>.json files. */', '#include "setup_portal.h"', '']
    for i, (code, (name, label, texts)) in enumerate(m.items()):
        js = json.dumps(texts, ensure_ascii=False, separators=(',', ':'), sort_keys=True)
        lines.append('static const char s_json_%d[] =\n    %s;' % (i, c_string(js)))
    lines.append('\nconst portal_lang_t portal_langs[] = {')
    for i, (code, (name, label, texts)) in enumerate(m.items()):
        lines.append('    { %s, %s, %s, s_json_%d, sizeof(s_json_%d) - 1 },' % (c_string(code), c_string(name), c_string(label), i, i))
    lines.append('};\nconst size_t portal_lang_count = sizeof(portal_langs) / sizeof(portal_langs[0]);\n')
    new = '\n'.join(lines)
    if not os.path.exists(out) or open(out, encoding='utf-8').read() != new:
        open(out, 'w', encoding='utf-8').write(new)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
