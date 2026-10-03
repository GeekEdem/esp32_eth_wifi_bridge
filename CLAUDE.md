# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

WT32-ETH01 (ESP32 + LAN8720) firmware that gives Wi-Fi to **one** Ethernet-only device (any device; a network printer is used for testing), or extends a wired network over Wi-Fi (access point mode). The firmware is device-agnostic: it forwards frames and never processes or rewrites the device's data; there is no device-specific logic or plan for it.

Written from scratch under the MIT license (`LICENSE`), inspired by martin-ger/esp32_eth_wifi_bridge (see Credits in `README.md`); the default branch is `main`. Docs, code and comments are in English. Every doc has a Ukrainian translation next to it named `<name>_UA.md` (e.g. `README_UA.md`); when a doc changes, update its `_UA` twin in the same commit. The web pages and the display are in English and Ukrainian (texts in `i18n/<code>.json`, see Key behaviours). Verified on hardware so far (WT32 up to 0.8.1, C3 0.3.1–0.4.2): the C3 programmer over RFC2217 (esptool flash/read, log, RTS resets, reconnects, the 0.4.2 EN watchdog; not its USB path or its own OTA), and on the WT32 Client mode, the page OTA with rollback confirmation and Berry scripts (checklist items 1, 3–5, 8, 10–12, 27–34 in `wt32/README.md`, marked with the firmware version; bench numbers under "Bench results"). Router/AP modes and the display/button are not; the WT32 must not be powered from the C3's 5 V (brownout loop, docs/WIRING.md); everything else is host-tested only — keep "built/tested on host" and "verified on the device" apart, and mark a checklist item only when the user reports it.

## Layout

```
wt32/                 main firmware (ESP-IDF project)
  main/main.c           mode dispatch, UART summary every 10 s
  main/settings.c       mode + own-network settings (NVS "wt32")
  main/eth.c            EMAC + LAN8720 (PHY addr 1, MDC 23, MDIO 18, power 16, clock in GPIO0)
  main/client_mode.c    client mode: station <-> Ethernet forwarding, "MGMT" netif on the shared IP
  main/l2rewrite.c      pure C: MAC rewriting in frames/ARP/DHCP, DHCP ACK snooping
  main/mgmt_demux.c     pure C: frame from Wi-Fi -> device / WT32 / both
  main/own_mode.c       router and access point modes: AP + Ethernet lwIP bridge; AP-mode uplink (DHCP client, fallback)
  main/dhcp_core.c      pure C: DHCP server logic (pool, leases, MAC reservations, save/load), DHCP probe
  main/dhcp_server.c    DHCP server task (UDP 67), leases in NVS "wt32"/"dhcp"
  main/display.c        SSD1306 (I2C, IO32/33) + button (IO4) task
  main/disp_ui.c        pure C: display pages into a 128x64 framebuffer; font6x10.c (misc-fixed, tools/gen_font.py)
  main/button.c         pure C: debounce + press length (short / 5 s / 10 s, acting on release)
  main/web.c, page.html web page (mode switch, per-mode status)
  main/i18n/            page texts per language (<code>.json)
  test/                 host tests (C, ASan/UBSan) + Playwright page test with a mock API
tools/c3-programmer/  ESP32-C3 SuperMini: RFC2217 + USB programmer/monitor for the WT32
  components/rfc2217-server  vendored igrr/rfc2217-server v0.4.0 + TCP keepalive, probe/disconnect (PATCHES.md)
shared/wifi_setup/    component used by both apps: Wi-Fi station + setup AP + captive DNS,
                      web server core (/api/status, scan, wifi, forget), page password (auth.js),
                      OTA upload + rollback confirmation, firmware info (ota.js), page languages:
                      i18n/*.json, tools/i18n_bundle.py, portal_i18n() in project_include.cmake
shared/script_berry/  Berry user script: VM task, memory/time limits, crash guard, storage partition,
                      /api/script* + script.js; berry/ is a git submodule (pinned commit)
shared/partitions/    4mb_ota.csv: nvs, otadata, ota_0/ota_1 (1.875 MB each), storage (128 KB, littlefs)
hardware/case/        3D-printed case: build_case.py (manifold3d CSG) -> stl/ in print orientation + check_report.txt;
                      WT32 sizes from the egnor/wt32-eth01 STEP (downloaded by --step, not committed: no license);
                      OLED/USB-C/switch are typical values; every check in the report must be "ok"
docs/WIRING.md        wiring diagrams (docs/wiring/gen_wiring.py -> SVG) and the first flash
docs/ARCHITECTURE.md  decisions, stages, open questions
docs/img/             README images: screenshots.js (Playwright against the mocks, both languages),
                      display_sheet.py (disp_ui_test .pbm screens -> display_<lang>.png); regenerate after UI changes
```

## Build

ESP-IDF >= 6.0 (tested on 6.1). Run `git submodule update --init` first (Berry); its constant tables are generated at build time by `berry/tools/coc`. The LAN87xx PHY driver comes from `espressif/esp-eth-drivers`; `mdns` is also pulled from git (not the component registry); `rfc2217-server` v0.4.0 is vendored in `tools/c3-programmer/components/rfc2217-server` with keepalive and probe/disconnect patches (`PATCHES.md`): keep its LICENSE and update it by hand.

```bash
. $IDF_PATH/export.sh
cd wt32 && ./build_firmware.sh                  # -> wt32/firmware/wt32-bridge.bin (flash at 0x0)
cd tools/c3-programmer && ./build_firmware.sh   # -> firmware/c3-programmer.bin
```

Prebuilt images are committed in each project's `firmware/`: `<name>.bin` (merged, flash at 0x0 over serial) and `<name>-ota.bin` (app only, for the web update). Rebuild them when the code changes, and bump `PROJECT_VER` for releases. The page shows the source commit (`git describe --always --dirty` at configure time): commit the code first, then build the images from a clean checkout of that commit (e.g. `git worktree add`), so the images do not say `-dirty` or `unknown` (the first project's new images make the tree dirty: `git checkout -- .` before building the second); commit the images afterwards.

## Tests

```bash
cd wt32/test
cc -Wall -Wextra -fsanitize=address,undefined -I../main -o /tmp/l2t l2rewrite_test.c ../main/l2rewrite.c && /tmp/l2t
cc -Wall -Wextra -fsanitize=address,undefined -I../main -o /tmp/dmx mgmt_demux_test.c ../main/mgmt_demux.c && /tmp/dmx
cc -Wall -Wextra -fsanitize=address,undefined -I../main -o /tmp/dct dhcp_core_test.c ../main/dhcp_core.c && /tmp/dct
cc -Wall -Wextra -fsanitize=address,undefined -I../main -o /tmp/btn button_test.c ../main/button.c && /tmp/btn
cc -Wall -Wextra -fsanitize=address,undefined -I../main -o /tmp/dui disp_ui_test.c ../main/disp_ui.c ../main/font6x10.c ../main/button.c && /tmp/dui [dir-for-pbm-screens]
python3 dhcp_scapy_test.py                         # needs scapy; builds dhcp_core.c as a shared lib
python3 mock_wt32.py ../main/page.html ../../shared/wifi_setup/auth.js 8811 &     # Playwright tests:
node mode_flow.js 8811 && node ota_flow.js 8811 ../firmware/wt32-bridge.bin ../firmware/wt32-bridge-ota.bin
node script_flow.js 8811 && node dhcp_flow.js 8811 && node ../../shared/wifi_setup/test/i18n_flow.js 8811 wt32
# (the mock keeps state: start a fresh one per test)
python3 ../../shared/wifi_setup/tools/i18n_bundle.py check --complete ../main/i18n ../../shared/wifi_setup/i18n ../../shared/script_berry/i18n \
  --sources ../main/page.html ../main/*.c ../../shared/wifi_setup/*.js ../../shared/wifi_setup/*.c ../../shared/script_berry/script.js ../../shared/script_berry/*.c
../../shared/script_berry/test/run_host_test.sh    # Berry with our berry_conf.h: prelude, limits, errors
# C3 page: shared/wifi_setup/test (mock_portal.py <page> <auth.js> <port> <app i18n dir> + login_flow.js, i18n_flow.js)
```

Keep pure logic (frame parsing, demux) free of ESP-IDF headers so it stays host-testable.

## Key behaviours

- **Client mode**: an 802.11 station may use only its own MAC, so device frames leave with the station MAC and MACs inside ARP/DHCP (chaddr, option 61 both ways) are swapped; the router sees one client under the station MAC. EMAC is promiscuous; IPv6 is dropped by default. The WT32 shares the device's IP and takes only new TCP connections to the management port (default 28480, kept below ephemeral ranges) and flows it opened itself; broadcast/multicast/ARP go to both. When the Wi-Fi TX buffers are full (`ESP_ERR_NO_MEM`) a frame from the device waits up to 5 ticks in the EMAC RX task; EMAC flow control (eth.c) then sends PAUSE frames to the device if it negotiated pause. The MGMT netif is a custom esp_netif: `esp_netif_set_mac()` is required (the config's `base.mac` never reaches lwIP's `hwaddr`, so frames would leave with a zero source MAC). The device's address (`l2rewrite.c`): its DHCP lease once the ACK passes, else the static source address, which moves only after 30 s of silence while another is in use; it counts as reachable if leased or another host of its subnet is heard on Wi-Fi (sticky per address), as unreachable only after the station has been up for 12 s with no DHCP exchange of the device in flight, and as unknown before that (setup AP up, no warning). The setup AP stays up while the network is unconfigured/unreachable, the device's IP is unknown, or it is not reachable (a static address from another network).
- **Modes**: `client`, `own` (shown as «Роутер»), `ap` («Точка доступу»); the two AP modes share the own_* settings (SSID, password, channel, IP).
- **Router mode (`own`)**: WPA2/WPA3 AP; AP and Ethernet in one lwIP bridge (static IP, WT32 at 192.168.77.1 by default); own DHCP server instead of IDF's dhcps: pool .100-.200, 2 h leases, up to 32 clients, router = WT32, no DNS; a MAC keeps its address; reservations MAC -> IP (any address in the subnet but the WT32's) from the page; leases saved to NVS every 30 s (remaining time, no wall clock), reservations immediately. Relayed requests (giaddr) are ignored.
- **Access point mode (`ap`)**: same bridge, but the bridge netif is a DHCP client of the network on the cable (router's DHCP passes through; no MAC rewriting). No address for 30 s -> DHCP client stopped, static fallback own_ip/24, and the DHCP server (disabled until then) serves only AP stations (`esp_wifi_ap_get_sta_list`), 2-min leases, not persisted. While on fallback, a probe DISCOVER (own UDP socket on 68, bound to the bridge) goes out every 30 s and on Ethernet link-up; an OFFER from another server -> server off, DHCP client on. Page on port 80 / `wt32.local`.
- **Display and button** (optional): SSD1306 128x64 at 0x3C/0x3D, dark after 60 s without a press; pages per mode + script outputs + system. Button acts on release: short = wake/next page, 5 s = one-time setup start (RTC_NOINIT flag + SW reset -> client mode with `ap_always_on`, saved mode untouched, page shows `setupBoot`), 10 s = `nvs_flash_erase()` + restart; a button held at power-on is ignored until released.
- **Web page**: port 80 (setup AP / own network) and the management port (client mode). Every API route except login (and the static files, `/i18n.json`) needs a session; default password `12345678`, salted SHA-256 in NVS, 30-min sessions, 30 s lockout after 5 failures. `/api/status` includes `build` (version, date/time, commit, ELF SHA-256, IDF).
- **Languages**: flat `i18n/<code>.json` per component (`en` is the reference; `_name`/`_label` once per language); `portal_i18n(<dirs>)` in the app's main CMakeLists merges them (missing keys from English) into `portal_langs[]`, served at `/i18n.json?l=`. The HTML is English with `data-i18n` / `data-i18n-ph` / `data-i18n-title`; JS uses `I18N.t(key, {params})` and redraws in `I18N.onChange`. The firmware never sends display text: errors are `setup_portal_send_error_key(req, "err.x", "English")` / `PORTAL_MSG`, the page shows `I18N.msg(j)`. Every new text or error key goes into `en.json` and `uk.json`; `i18n_bundle.py check --complete --sources …` must pass. The device's language: NVS `wifi_setup`/`lang`, `POST /api/lang` (session), `setup_portal_lang()`, `on_lang` callback; `/i18n.json` without `l` answers in it (else `?b=` browser hint, else the first). auth.js sends a switch to the device (after login if made before), and the first login saves the page's language if none is saved. The display texts are `disp.*` keys in `wt32/main/i18n` (21-character lines, checked); `disp_ui.c` reads them from the bundled JSON (`ui_set_texts`, `ui_tr`, `ui_trf` with `{name}` pairs), the display task switches via `display_set_texts()`. `disp_ui_test` reads `../main/i18n/*.json`: run it from `wt32/test`.
- **OTA**: POST /api/ota with the app image; header checked (magic, chip id, project name) before any flash write; sequential writes into the inactive slot; the new image is confirmed (`esp_ota_mark_app_valid_cancel_rollback`) after 60 s with the web server up, otherwise the bootloader rolls back.
- **Scripts (Berry)**: one user script from the "storage" partition, compiled from a flash mapping by the script task (core 1, low priority). Allocations go through a budget allocator (default 40 KB; beyond it Berry returns BE_MALLOC_FAIL), the VM heartbeat hook raises `timeout_error` after 2 s of one run, any error stops the script, and an abnormal reset while script code runs (RTC_NOINIT marker) turns autostart off. Only the script task maps or writes the partition; the page reads it in chunks. API: print, output, every/after/cancel, status (the page's status JSON), millis, heap.
- **C3 programmer**: RTS -> EN, DTR -> IO0, both open-drain (IO0 is the WT32's Ethernet clock input after boot; never drive it high). IO0 is held low 50 ms after EN release. `miniterm` over RFC2217 needs `--rts 0 --dtr 0`. rfc2217-server v0.4.0 hands `on_purge` the raw RFC 2217 value (1 receive, 2 transmit, 3 both; its enum is 0/1/2) and pyserial wants it echoed back; the receive purge is done by `uart_rx_task` (flag + semaphore), never `uart_flush_input()` from the server thread. The library ignores `task_priority`: its pthreads get the priority from `esp_pthread_set_cfg` (8), the UART/USB pumps run at 5. A client that vanished is dropped after ~15 s (`C3PROG_CLIENT_KEEPALIVE_S`): after 10 s of silence bridge.c sends a telnet DO TIMING-MARK probe (pyserial answers WONT) and calls `rfc2217_server_disconnect()` if nothing comes within 5 s; TCP keepalive alone (0.4.2) does not act while unacknowledged data is in flight (lwIP retransmits instead, measured 31–34 s), and EN held > 3 s with no control request is released with IO0 (`C3PROG_EN_HOLD_MAX_MS`, watchdog in target_ctl.c).
