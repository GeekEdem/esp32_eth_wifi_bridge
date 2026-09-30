# Shared by the page mocks: /i18n.json and keyed errors as the device sends
# them (setup_portal.c), from the same i18n/<code>.json files as the firmware.
import os, sys, urllib.parse
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'tools'))
import i18n_bundle

BUILD = {"project": "mock", "version": "0.1.0", "date": "Sep 30 2026", "time": "12:00:00",
         "commit": "0123456789", "elf": "89abcdef01234567", "idf": "v6.1"}


class Langs:
    def __init__(self, dirs):
        langs, errors = i18n_bundle.load(list(dirs) + [os.path.join(HERE, '..', 'i18n')])
        errors += i18n_bundle.validate(langs)[0]
        if errors:
            raise SystemExit('\n'.join(errors))
        self.m = i18n_bundle.merged(langs)
        self.en = self.m['en'][2]
        self.saved = None                   # the device's language (NVS), None = not saved

    def response(self, path):
        """Body of GET /i18n.json: ?l=<code>, else the saved language, else ?b=<code>
        (the browser's), else the first (as setup_portal.c)."""
        q = urllib.parse.parse_qs(urllib.parse.urlparse(path).query)
        first = next(iter(self.m))
        device = self.saved or first
        code = q.get('l', [''])[0]
        if code not in self.m:
            code = q.get('b', [''])[0] if not self.saved else ''
        if code not in self.m:
            code = device
        return {"lang": code, "device": device, "saved": self.saved is not None,
                "langs": [{"code": c, "name": n, "label": l} for c, (n, l, _) in self.m.items()],
                "strings": self.m[code][2]}

    def post_lang(self, form):
        """POST /api/lang (after the session check): (status, body)."""
        code = form.get('lang', '')
        if code not in self.m:
            return 400, self.err('err.unknownLanguage')
        self.saved = code
        return 200, {"ok": True}

    def err(self, key, **extra):
        """{"ok":false,"key":...,"message":English} as setup_portal_send_error_key() sends it."""
        if key not in self.en:
            raise KeyError(key)
        return dict({"ok": False, "key": key, "message": self.en[key]}, **extra)
