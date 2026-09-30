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

    def response(self, path):
        """Body of GET /i18n.json?l=<code>: unknown code -> the first language."""
        q = urllib.parse.parse_qs(urllib.parse.urlparse(path).query)
        code = q.get('l', [''])[0]
        if code not in self.m:
            code = next(iter(self.m))
        return {"lang": code, "langs": [{"code": c, "name": n, "label": l} for c, (n, l, _) in self.m.items()],
                "strings": self.m[code][2]}

    def err(self, key, **extra):
        """{"ok":false,"key":...,"message":English} as setup_portal_send_error_key() sends it."""
        if key not in self.en:
            raise KeyError(key)
        return dict({"ok": False, "key": key, "message": self.en[key]}, **extra)
