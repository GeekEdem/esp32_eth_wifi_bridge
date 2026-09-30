# Login and language tests

*[Українська](README_UA.md)*

The WT32 page (with modes) has its own tests in `wt32/test/`; `i18n_flow.js` from here runs against both pages.

`auth.js` + the app page against a mock API (`mock_portal.py` mimics the behaviour of `setup_portal.c` / `portal_auth.c`; `mock_i18n.py` serves `/i18n.json` and keyed errors from the same `i18n/<code>.json` files the firmware is built with):

- `login_flow.js`: a single login form for parallel 401s, wrong password, login with `12345678`, the default-password warning, password change, logout and login with the new password;
- `i18n_flow.js`: the browser's language while the device has none, the first login saving it on the device, the device's language for a new browser, the switch on the login form (sent to the device after the login), a device error in the chosen language, no English left on the page in Ukrainian and no Cyrillic in English, the choice kept after a reload, the firmware info (version, build time, commit, SHA-256).

```bash
cd shared/wifi_setup/test
python3 mock_portal.py ../../../tools/c3-programmer/main/portal.html ../auth.js 8802 ../../../tools/c3-programmer/main/i18n &
node login_flow.js 8802 c3     # needs playwright; the mock keeps state: restart it before the next test
kill %1
python3 mock_portal.py ../../../tools/c3-programmer/main/portal.html ../auth.js 8802 ../../../tools/c3-programmer/main/i18n &
node i18n_flow.js 8802 c3
kill %1
```
`CHROMIUM=/path/to/chrome` — if Playwright should use an already installed browser.

The language files themselves: `python3 ../tools/i18n_bundle.py check --complete <i18n folders> --sources <pages and sources>` (the full command for each app is in its README).
