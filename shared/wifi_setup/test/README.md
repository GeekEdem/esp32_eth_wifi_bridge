# Login page test

*[Українська](README_UA.md)*

The WT32 page (with modes) has its own test — `wt32/test/mode_flow.js`.

`auth.js` + the app page against a mock API (`mock_portal.py` mimics the behaviour of `setup_portal.c` / `portal_auth.c`): a single login form for parallel 401s, wrong password, login with `12345678`, the default-password warning, password change, logout and login with the new password.

```bash
cd shared/wifi_setup/test
python3 mock_portal.py ../../../tools/c3-programmer/main/portal.html ../auth.js 8802 &
node login_flow.js 8802 c3     # needs playwright
kill %1
```
`CHROMIUM=/path/to/chrome` — if Playwright should use an already installed browser.
