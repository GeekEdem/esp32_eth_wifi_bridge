# Тест сторінки входу

*[English](README.md)*

Сторінку WT32 (з режимами) перевіряє власний тест — `wt32/test/mode_flow.js`.

`auth.js` + сторінка застосунку проти імітації API (`mock_portal.py` повторює поведінку `setup_portal.c` / `portal_auth.c`): одна форма входу на паралельні 401, невірний пароль, вхід з `12345678`, попередження про стандартний пароль, зміна пароля, вихід і вхід з новим.

```bash
cd shared/wifi_setup/test
python3 mock_portal.py ../../../tools/c3-programmer/main/portal.html ../auth.js 8802 &
node login_flow.js 8802 c3     # потрібен playwright
kill %1
```
`CHROMIUM=/шлях/до/chrome` — якщо Playwright має використати вже встановлений браузер.
