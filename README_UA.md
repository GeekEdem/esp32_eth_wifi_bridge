# WT32-ETH01: Wi-Fi для одного Ethernet-пристрою

*[English](README.md)*

Прошивка для **WT32-ETH01** (ESP32 + LAN8720), яка дає Wi-Fi пристрою з одним лише Ethernet-портом (будь-якому: принтеру, контролеру, NAS тощо) або роздає по Wi-Fi мережу з кабелю. Прошивка не знає нічого про сам пристрій і не змінює його дані: вона лише пересилає кадри.

> Статус: усе зібрано й перевірено тестами на ПК; **на залізі ще не перевірено**.

## Режими

| Режим | Як працює | Сторінка WT32 |
|---|---|---|
| **Клієнт** (за замовчуванням) | WT32 підключається до наявного Wi-Fi; пристрій отримує IP від роутера цієї мережі й доступний, ніби під'єднаний до нього кабелем | `http://<IP пристрою>:28480` |
| **Роутер** | WT32 роздає свій Wi-Fi з DHCP; пристрій і телефони — в одній мережі, без інтернету | `http://192.168.77.1` |
| **Точка доступу** | кабель WT32 — у роутер; WT32 роздає його мережу по Wi-Fi (DHCP, інтернет — від роутера) | `http://wt32.local` або адреса з роутера |

Перше налаштування — через точку `WT32-Setup-XXXX`, сторінка під паролем (за замовчуванням `12345678`).

## Структура

| Тека | Що це |
|---|---|
| [`wt32/`](wt32/) | основна прошивка WT32-ETH01; готовий образ у `wt32/firmware/`, інструкція й чекліст перевірки — [`wt32/README_UA.md`](wt32/README_UA.md) |
| [`tools/c3-programmer/`](tools/c3-programmer/) | ESP32-C3 SuperMini як програматор і монітор WT32 по Wi-Fi (RFC2217) або USB |
| [`shared/wifi_setup/`](shared/wifi_setup/) | спільний компонент: Wi-Fi з налаштуваннями в NVS, точка налаштування з captive portal, веб-API, пароль сторінки, OTA |
| [`shared/script_berry/`](shared/script_berry/) | скрипти користувача на Berry: редактор на сторінці, ліміти памʼяті й часу |
| [`hardware/case/`](hardware/case/) | корпус для друку (STL + генератор на Python з перевірками проти 3D-моделі плати): WT32, екран, кнопка, живлення USB-C |
| [`docs/ARCHITECTURE_UA.md`](docs/ARCHITECTURE_UA.md) | рішення, етапи, відкриті питання |

## Швидкий старт

1. Прошити C3 образом `tools/c3-programmer/firmware/c3-programmer.bin` (з адреси `0x0`), підключити його до Wi-Fi через `C3prog-Setup-XXXX`.
2. З'єднати C3 з WT32 (схема — у [README програматора](tools/c3-programmer/README_UA.md)) і прошити WT32:
   ```bash
   esptool.py --chip esp32 -p rfc2217://c3prog.local:4000 write_flash 0x0 wt32/firmware/wt32-bridge.bin
   ```
3. Підключитися до `WT32-Setup-XXXX`, обрати режим і мережу.

## Збірка

Після клонування: `git submodule update --init` (Berry). ESP-IDF ≥ 6.0 (перевірено на 6.1). У кожному проєкті: `./build_firmware.sh` — чиста збірка, повний образ для запису кабелем з `0x0` і `…-ota.bin` для оновлення через сторінку.

## Подяки

Написано з нуля; натхненно ідеями й напрацюваннями:

- [martin-ger/esp32_eth_wifi_bridge](https://github.com/martin-ger/esp32_eth_wifi_bridge) від Martin Gergeleit — оригінальний міст ESP32 Ethernet ↔ Wi-Fi AP, з якого почався цей проєкт; режими «Роутер» і «Точка доступу» використовують ту саму ідею (lwIP-міст Ethernet і точки доступу), реалізовану тут заново;
- [martin-ger/esp32_nat_router](https://github.com/martin-ger/esp32_nat_router), з якого виріс той проєкт;
- приклад ESP-IDF [`examples/network/sta2eth`](https://github.com/espressif/esp-idf/tree/master/examples/network/sta2eth) — підміна MAC пристрою на MAC станції для режиму «Клієнт»;
- [egnor/wt32-eth01](https://github.com/egnor/wt32-eth01) — нотатки про WT32-ETH01 і її 3D-модель, за якою перевіряється корпус (завантажується під час збірки, у репозиторій не входить).

Сторонні частини мають власні ліцензії: [Berry](https://github.com/berry-lang/berry) (MIT, git-субмодуль), шрифт misc-fixed 6×10 (public domain, з [u8g2](https://github.com/olikraus/u8g2)); ESP-IDF, esp-protocols (mdns), esp-eth-drivers і rfc2217-server (Apache-2.0) завантажуються під час збірки.

## Ліцензія

[MIT](LICENSE).
