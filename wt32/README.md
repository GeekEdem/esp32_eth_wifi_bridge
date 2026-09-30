# WT32-ETH01: Wi-Fi for one Ethernet device

*[Українська](README_UA.md)*

Main firmware. The WT32 is connected by cable to **one** device (any device) and gives it Wi-Fi (or, in Access point mode («Точка доступу»), shares the router's network from the cable over Wi-Fi). The firmware knows nothing about the device and does not change its data — it only forwards frames.

> Status: built (ESP-IDF 6.1); the forwarding, management port and script logic is tested on a PC, the page in a browser against a mock API. **Not verified** on hardware.

## Three modes (chosen on the page, applied after a restart)

### Client («Клієнт», default)
The WT32 connects to an existing Wi-Fi network. The device gets an IP from that network's router and is reachable as if it were plugged into the router with a cable.

- A Wi-Fi client can have only one MAC, so the device's frames leave with the **WT32 station MAC**, and the MACs inside ARP and DHCP (chaddr, option 61) are rewritten. The router sees one client — under the WT32 station MAC (printed in the log at startup). Reserve the IP for this MAC on the router.
- The WT32 has no IP of its own: it uses the device's IP and takes only new TCP connections to port **28480** and connections it opened itself. Broadcast, multicast and ARP go to both; everything else (the device's services on any port, its web page on 80, ping) goes to the device. Netmask, gateway and DNS come from the router's DHCP reply to the device.
- Page: **`http://<device IP>:28480`**. While the network is not configured or has been unreachable for 60 s, or the device's IP is not known yet, the page is also available through the **`WT32-Setup-XXXX`** access point → http://192.168.4.1 (open, no Wi-Fi password).
- IPv6 is not forwarded (MACs in neighbor discovery packets are not rewritten).

### Router («Роутер»)
The WT32 runs its own Wi-Fi network (WPA2/WPA3). The access point and Ethernet are one network with a DHCP server on the WT32: the device on the cable and phones get addresses and see each other. There is no internet (NAT is not needed: there is nowhere to go).

- WT32 — `192.168.77.1` (configurable), DHCP pool — `.100–.200`; page — `http://192.168.77.1` or `http://wt32.local`.
- The device's IP is shown on the page (the WT32 remembers its MAC from the first frame and looks up the address it was given).
- The DHCP server is our own (`dhcp_core.c`): 2 h leases, up to 32 clients; a client gets the same address for its MAC. The network addresses section («Адреси в мережі (DHCP)») of the page lists the clients (the device on the cable is marked) and **IP reservations by MAC** (with a button in the client's row or manually; any address in the subnet except the WT32's). Leases and reservations are stored in NVS and survive a restart; reservations are saved immediately, leases every 30 s.
- DHCP hands out the WT32's address as the gateway and no DNS (there is no internet).

### Access point («Точка доступу»)
The WT32's cable goes into a router (or a switch on a network with DHCP), and the WT32 shares that network over Wi-Fi. The access point and Ethernet are one bridge: addresses (DHCP), internet, device discovery — all come from the router; the WT32 changes nothing.

- The WT32 is one more DHCP client on that network (hostname `wt32`); page — `http://<WT32 address>` (shown in the router's client list and on UART) or `http://wt32.local`.
- **No DHCP on the cable for 30 s** (cable unplugged, router rebooting) → the WT32 takes a fallback address (the Fallback WT32 address field («Запасна адреса WT32»), default `192.168.77.1`) and itself hands out `.100–.200` **only to clients of its own access point**, with 2-min leases, so the page stays reachable. Meanwhile, every 30 s and when the cable is plugged in, the WT32 sends a probe DHCP request; as soon as the router answers, it returns to the router's DHCP, and clients move to the router's addresses within 1–2 min.
- Wi-Fi name, password and channel are shared with Router mode.

## Web page

Password-protected: default **`12345678`**, changed in the Page password section («Пароль сторінки») (8–63 characters); while the default is set, the page shows a reminder. A session lasts until 30 min of inactivity; 5 failed attempts in a row lock login for 30 s.

- **Status** («Стан»): mode, Wi-Fi, Ethernet, device MAC/IP, page address; in Client mode — traffic counters with explanations.
- **Mode** («Режим»): the Client / Router / Access point switch («Клієнт» / «Роутер» / «Точка доступу»); for Client — Wi-Fi network selection and password; for Router and Access point — name, password (8–63), channel, WT32 address (for Access point — the fallback one).
- Forget device («Забути пристрій (після заміни)») — after replacing the device on the cable (Client). Forget client Wi-Fi («Забути Wi-Fi клієнта») — returns to the setup access point.

**Forgot the page password or the WT32 network password** → hold the button for 10 s (see below). Without a button — erase the flash and flash again (this erases all settings):
```bash
esptool.py --chip esp32 -p rfc2217://c3prog.local:4000 erase_flash
esptool.py --chip esp32 -p rfc2217://c3prog.local:4000 write_flash 0x0 firmware/wt32-bridge.bin
```

## Display and button (optional)

A 0.96″ 128×64 I2C OLED (SSD1306) and one button. Everything works without them; the button also works without the display.

| What | WT32 pin |
|---|---|
| OLED VCC | **3V3** (not 5 V: the I2C pull-ups on the module go to its supply) |
| OLED GND | GND |
| OLED SDA | IO32 (labelled CFG on the board) |
| OLED SCL | IO33 (labelled 485_EN on the board) |
| Button | between **IO4** and GND (internal pull-up, no resistor needed) |

- The display is detected automatically at address 0x3C or 0x3D (in the log: `SSD1306 at 0x3C`). It turns off after 60 s without a press (an OLED burns in from a static image).
- Pages: **Network** («Мережа»: mode, Wi-Fi, WT32 page address), **Device** («Пристрій»: Ethernet, MAC, IP; in Access point mode — **Cable** («Кабель»): router DHCP, gateway), **Traffic** («Трафік», Client), **Script** («Скрипт»: the script's first 5 `output()` values, if any), **System** («Система»: version, memory, uptime).
- **Button** (acts on release; while you hold it, the display shows what will happen):
  - short — turn the display on / next page;
  - **5 s** — one-time setup start: the WT32 restarts in Client mode with the `WT32-Setup-XXXX` access point (http://192.168.4.1). The saved mode is not changed: after the next restart the WT32 returns to it, unless something else has been saved on the page;
  - **10 s** — reset: erases all settings (mode, networks, page password → `12345678`, DHCP leases, script autostart); the script text is kept;
  - 1–5 s — nothing (cancel). A button held down at power-on is ignored until it is released.

## Scripts (Berry)

The Script (Berry) section («Скрипт (Berry)») of the page: an editor, Save and run («Зберегти й запустити»), Stop («Зупинити»), Run at startup («Запускати при старті»), the Results table («Результати») and a console. A script can read the device status (`status()`), output results (`output()`) and run on timers (`every`, `after`). API and limits — [`../shared/script_berry/README.md`](../shared/script_berry/README.md).

- Script memory budget is 40 KB, a single run of code is limited to 2 s; an error stops only the script.
- If the device restarted because of a crash while the script was running, autostart is turned off.
- Cost in the firmware: ~102 KB of flash, ~6.5 KB of static RAM; while running, another 10 KB of task stack plus the script budget.

## Firmware update from the page (OTA)

The Firmware update section («Оновлення прошивки») of the page: file **`firmware/wt32-bridge-ota.bin`** → Update firmware («Оновити прошивку»). The page rejects the full image `wt32-bridge.bin` (for address `0x0`) — it is only for flashing over the cable.

- Before writing, the image is checked to be for this chip and this firmware; after writing, its integrity is checked.
- The flash has two slots (`ota_0`/`ota_1`, 1.875 MB each); the new firmware is written to the inactive one, the old one stays.
- After the restart the new firmware is **on probation for 60 s**: if it crashes or restarts during that time, the bootloader goes back to the previous one. The page shows which version is actually running.
- Sessions do not survive a restart — the page will ask you to log in again.
- The first time, firmware with this partition layout has to be written over the cable (full image at `0x0`); the settings (NVS) are kept.

## 1. Flash the WT32 via the C3

Wiring diagrams and the first flash step by step: [`docs/WIRING.md`](../docs/WIRING.md).

C3 ↔ WT32 wiring — see [`../tools/c3-programmer/README.md`](../tools/c3-programmer/README.md).

```bash
esptool.py --chip esp32 -p rfc2217://c3prog.local:4000 write_flash 0x0 firmware/wt32-bridge.bin
```

Log (a mode summary every 10 s):
```bash
python -m serial.tools.miniterm --rts 0 --dtr 0 rfc2217://c3prog.local:4000 115200
```
`--rts 0 --dtr 0` is required: otherwise RTS holds the WT32 in reset.

## 2. First setup

1. Connect a phone to **`WT32-Setup-XXXX`** → the page opens (or http://192.168.4.1), password `12345678`.
2. Client mode: choose the home network, enter its password → Save and restart («Зберегти і перезапустити»).
   Or Router / Access point: name, password, channel → Save and restart.
3. Change the page password.

## 3. Checklist with a device

Any network device in **DHCP** mode (a printer is used for testing). A patch cord between the device and the WT32. "Normal use" means whatever the device is on the network for (a printer — printing from a PC, a camera — video, etc.).

**Client mode**

| # | Action | Expected | Result |
|---|---|---|---|
| 1 | Turn the device on | Ethernet — link up («є лінк»), the device MAC appears, then its IP | |
| 2 | Router's client list | a new client with the **WT32 station MAC** and the IP shown on the page | |
| 3 | From a PC: `ping <IP>` | replies | |
| 4 | Normal use of the device at `<IP>` | works | |
| 5 | Longer transfer (large file, several jobs in a row) | works without interruptions | |
| 6 | Vendor utility → search for the device on the network | finds it / doesn't (write down) | |
| 7 | Turn the device off and on | same IP, normal use works | |
| 8 | Restart the WT32 (device stays on) | connection comes back without restarting the device | |
| 9 | Restart the router | connection comes back by itself | |
| 10 | From a PC open `http://<IP>:28480`, log in | WT32 page; the management traffic counter («Службовий трафік») grows; the `WT32-Setup` access point is gone | |
| 11 | Use the device (item 4) with the page from item 10 open | works, the page keeps updating | |
| 12 | If the device has a web page: `http://<IP>` | the **device's** page opens, not the WT32's | |
| 13 | Change the page password, log out, log in with the new one | works; the old password is rejected | |

**Router mode**

| # | Action | Expected | Result |
|---|---|---|---|
| 14 | Switch to Router, save | WT32 restarts with the new network | |
| 15 | Connect a phone to it, `http://192.168.77.1` | WT32 page, mode "router" | |
| 16 | Page → device IP | an address from `.100–.200` | |
| 17 | Laptop on the same network: normal use of the device at `<IP>` | works | |
| 18 | Network addresses section («Адреси в мережі (DHCP)»): Pin («Закріпити») in the device's row | pinned («закріплено») | |
| 19 | Restart the WT32 and the device | the device gets the same address, the reservation is still there | |
| 20 | Pin manually («Закріпити вручну»): device MAC + `192.168.77.50`, restart the device | the device is on `.50` | |

**Access point mode** (the WT32's cable goes into a LAN port of the home router, not into the device)

| # | Action | Expected | Result |
|---|---|---|---|
| 21 | Page → Access point, save; cable into the router | WT32 restarts, network with the same name | |
| 22 | Connect a phone to the WT32 network | address from the router (as at home), internet works | |
| 23 | `http://wt32.local` or the WT32 address from the router's client list | page: "WT32 address … (from router)" («Адреса WT32 … (від роутера)»), gateway — the router | |
| 24 | Unplug the cable, after 1 min reconnect the phone to the WT32 network | the phone gets `192.168.77.1xx`; page at `http://192.168.77.1`, "fallback: no DHCP on the cable" («запасна: на кабелі немає DHCP») | |
| 25 | Plug the cable back in | within ≤ 30 s the WT32 has an address from the router again; the phone within 1–2 min (or reconnect it) | |
| 26 | Switch back to Client | connects to the home Wi-Fi | |

**Update**

| # | Action | Expected | Result |
|---|---|---|---|
| 27 | Page → Firmware update → `wt32-bridge.bin` | rejected: "not an update file, an «…-ota.bin» image is needed" («Це не файл оновлення. Потрібен образ «…-ota.bin»…») | |
| 28 | Same with `wt32-bridge-ota.bin` | progress, restart, login, "Updated…" («Оновлено…»), the partition has changed (`ota_0` ↔ `ota_1`) | |
| 29 | 1–2 min after item 28, restart the WT32 (power off) | starts from the new firmware (it has been confirmed) | |

**Scripts**

| # | Action | Expected | Result |
|---|---|---|---|
| 30 | Script (Berry) → example → Save and run | state "running" («працює»); Results shows mode, Ethernet, device IP, memory; updates every 5 s | |
| 31 | Use the device while the script runs | works as usual | |
| 32 | Script `while true end` | "error — timeout_error…" («помилка — timeout_error…»); the page and the device keep working | |
| 33 | Turn on Run at startup, restart the WT32 | the script starts by itself | |
| 34 | UART log, `script` line / Script memory («Памʼять скрипта») on the page | write down the device's free memory with the example running | |

**Display and button** (wiring — above)

| # | Action | Expected | Result |
|---|---|---|---|
| 35 | Power on the WT32 with the display | log `SSD1306 at 0x3C` (or 0x3D); the display shows the Network page, text is readable, not mirrored | |
| 36 | Press the button briefly, repeatedly | pages cycle, the number is in the title | |
| 37 | Leave it for 1 min; then press | the display turns off; the first press only turns it on | |
| 38 | Hold for 3 s and release | the display shows "Hold until 5 s…" («Тримайте до 5 с…») and a bar; nothing happens after release | |
| 39 | Hold for 6 s and release | restart; `WT32-Setup-XXXX` access point, the page shows "One-time setup start" («Разовий запуск для налаштування»); another restart — the saved mode | |
| 40 | Hold for 11 s and release | "RESET…" («СКИДАННЯ…»), restart; Client mode with no network, page password `12345678` | |

If something does not work, attach the filled-in tables, screenshots of the page and a few lines of the log to an issue.

**Hints on the Traffic page** (Client):
- "Dropped: Wi-Fi not connected" («Відкинуто: Wi-Fi не підключено») grows — the WT32 cannot hold Wi-Fi (check RSSI);
- "Frames from a second device" («Кадри від другого пристрою») > 0 — more than one device on Ethernet (a switch?) — only one is supported;
- "DHCP rewrites" («DHCP-перезаписи») = 0 and no IP — the device does not send DHCP (a static IP from another subnet?);
- "Transmit errors" («Помилки передачі») grow — open an issue with the UART log.

## Building from source

```bash
git submodule update --init       # Berry (build_firmware.sh does this too)
. $IDF_PATH/export.sh            # ESP-IDF ≥ 6.0 (LAN8720 driver comes from esp-eth-drivers)
cd wt32
./build_firmware.sh              # → firmware/wt32-bridge.bin (cable, 0x0) and wt32-bridge-ota.bin (page)
```

## Tests on a PC

```bash
cd wt32/test
# frame rewriting and management traffic parsing
cc -Wall -Wextra -fsanitize=address,undefined -I../main -o /tmp/l2t l2rewrite_test.c ../main/l2rewrite.c && /tmp/l2t
cc -Wall -Wextra -fsanitize=address,undefined -I../main -o /tmp/dmx mgmt_demux_test.c ../main/mgmt_demux.c && /tmp/dmx
# Berry: example, timers, time and memory limits, syntax errors
../../shared/script_berry/test/run_host_test.sh
# button (debounce, press length, held at startup) and display (UTF-8, every page of every mode);
# with a directory argument, disp_ui_test writes each screen to a .pbm file for viewing
cc -Wall -Wextra -fsanitize=address,undefined -I../main -o /tmp/btn button_test.c ../main/button.c && /tmp/btn
cc -Wall -Wextra -fsanitize=address,undefined -I../main -o /tmp/dui disp_ui_test.c ../main/disp_ui.c ../main/font6x10.c ../main/button.c && /tmp/dui
# DHCP server: packet parsing, hundreds of thousands of random requests (ASan), lease saving
cc -Wall -Wextra -fsanitize=address,undefined -I../main -o /tmp/dct dhcp_core_test.c ../main/dhcp_core.c && /tmp/dct
# DHCP over the protocol: scapy packets (DISCOVER/REQUEST/RENEW/NAK/RELEASE/DECLINE/INFORM, reservations); needs scapy
python3 dhcp_scapy_test.py
# page (needs playwright): login and modes; update — rejection / update / rollback; script; DHCP
python3 mock_wt32.py ../main/page.html ../../shared/wifi_setup/auth.js 8811 &
node mode_flow.js 8811
node ota_flow.js 8811 ../firmware/wt32-bridge.bin ../firmware/wt32-bridge-ota.bin
node script_flow.js 8811
node dhcp_flow.js 8811
kill %1
```

## Layout

| File | What it does |
|---|---|
| `main/main.c` | mode selection at startup, log, button actions (one-time setup start, reset) |
| `main/settings.c` | mode and WT32 access point settings in NVS |
| `main/eth.c` | EMAC + LAN8720 (WT32-ETH01) |
| `main/client_mode.c` | Client mode: forwarding, management interface on the shared IP |
| `main/l2rewrite.c` | MAC rewriting in frames, ARP, DHCP; DHCP ACK snooping |
| `main/mgmt_demux.c` | who gets a frame from Wi-Fi: the device, the WT32 or both |
| `main/own_mode.c` | Router and Access point modes: AP + Ethernet in a bridge; DHCP client with a fallback address |
| `main/dhcp_core.c` | DHCP server logic (no ESP-IDF): pool, leases, reservations, saving; probe DHCP request |
| `main/dhcp_server.c` | DHCP server: UDP 67, saving to NVS |
| `main/display.c` | SSD1306 display (I2C) and button: the task that draws the pages |
| `main/disp_ui.c`, `main/font6x10.c` | display pages (no ESP-IDF); 6×10 font with Cyrillic (misc-fixed, public domain; `tools/gen_font.py`) |
| `main/button.c` | button (no ESP-IDF): debounce and press length |
| `main/web.c`, `main/page.html` | web page; Wi-Fi, password, OTA and portal — `shared/wifi_setup`; scripts — `shared/script_berry` |
