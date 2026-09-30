# Architecture: a universal Ethernet ↔ Wi-Fi adapter on the WT32-ETH01

*[Українська](ARCHITECTURE_UA.md)*

This document records the agreed decisions; the **[to verify]** marker means an assumption not yet confirmed on the device.

## Goal

The WT32-ETH01 is connected by cable to **one** Ethernet end device (any device; a network printer is used for testing) and gives it Wi-Fi. The firmware is universal: it knows nothing about the device and never processes or changes its data — it only forwards frames (in client mode it rewrites MAC addresses in frame headers and in ARP/DHCP, never the payload).

## Modes (selected in the web portal)

### 1. Router (formerly "Own network" («Власна мережа»))
The WT32 is an access point with a DHCP server. The Ethernet device and the Wi-Fi clients are in one subnet (e.g. `192.168.77.0/24`, WT32 = `.1`), as behind an ordinary router, but without internet.
- Implementation: `wt32/main/own_mode.c`: an lwIP bridge ETH ↔ AP (the approach of [martin-ger/esp32_eth_wifi_bridge](https://github.com/martin-ger/esp32_eth_wifi_bridge), re-implemented); the device's IP is shown on the page (MAC from the first frame → leased address).
- Own DHCP server instead of the one built into ESP-IDF (which has no static leases and keeps addresses only in RAM): `dhcp_core.c` (pure C, host tests) + `dhcp_server.c` (UDP 67, NVS). Pool `.100–.200`, 2 h leases, up to 32 clients, gateway = WT32, no DNS is handed out. IP reservations by MAC are set on the page. Leases are stored as remaining time (there is no clock).

### 2. Client (transparent)
The WT32 joins an existing Wi-Fi network; the Ethernet device gets an IP from that network's router and is visible on the network as if it were connected by cable.
- Based on the logic of the ESP-IDF example `examples/network/sta2eth`: the device's MAC is rewritten to the station MAC (including DHCP chaddr/option 61 and ARP). Implementation: `wt32/main/client_mode.c` + `l2rewrite.c`, with fixes for our case: promiscuous Ethernet, device MAC taken from the first frame, DHCP rewriting in both directions, no getting stuck in setup mode.
- Wi-Fi limitation: the client presents one MAC to the access point → **only one device** on Ethernet. A second MAC → warning.
- IPv6 ND carries the MAC in the packet body → rewrite or block **[to verify]**.

### 3. Access point
The WT32's cable goes into a router; the WT32 shares that network over Wi-Fi. The same ETH ↔ AP bridge as in "Router", but addresses, DHCP and internet come from the router (frames are not modified; this is what [martin-ger/esp32_eth_wifi_bridge](https://github.com/martin-ger/esp32_eth_wifi_bridge) does).
- The WT32 is a DHCP client on the bridge; the page is at its address (port 80) or `wt32.local`.
- Fallback: 30 s without an address → a static fallback address (a settings field, default `192.168.77.1/24`) and our DHCP server for access point clients only (2 min leases, not persisted), so the page stays reachable. Every 30 s and on link-up, a probe DISCOVER; a reply from another server → back to DHCP.
- A separate setup access point alongside is impossible (the ESP32 has one AP); the page is reachable from the router's network or via the fallback address.
- **[to verify]** broadcast sending from the socket goes through the bridge; the router answers the probe; return to DHCP without a restart.
- A real NAT router (internet from the cable, own subnet over Wi-Fi) is out of scope: one device does not need it.

### Setup portal
Access point `WT32-Setup-XXXX` with a captive portal: on first start, when Wi-Fi is unavailable, or after holding the button for 5 s (a one-time setup start).

## Managing the ESP in transparent mode: shared IP + management port

The ESP has no IP of its own — it uses the device's IP (learned from DHCP/ARP) and intercepts only its own traffic:
- a new TCP connection (SYN) to the management port → ESP;
- packets of connections opened by the ESP (connection table) → ESP;
- broadcast/multicast → both the ESP and the device;
- everything else → the device.

Connection tracking is needed so as not to intercept a device connection that happens to use the same port number.

The management port lies outside the ephemeral ranges (Linux 32768–60999, Windows/lwIP 49152–65535) and is not a popular one; the default is, e.g., **28480**, changeable in the portal. The ESP's own outgoing connections (NTP, OTA, script) use a reserved range of local ports.
Precedent: Intel AMT shares the IP with the host and takes ports 16992–16995.

Implemented in `wt32/` (`mgmt_demux.c` + a separate netif "MGMT" with the station MAC and the device's IP; netmask/gateway/DNS from the DHCP ACK the device receives). The portal's web server listens on 80 (setup access point) and on the management port (home network).

Caveat: the ESP cannot reach the device at the device's own IP (to the ESP it is its own address). For scripts: an internal link-local address that the ESP intercepts on the Ethernet side **[to verify that the device replies via the gateway]**.

## Display and button (optional) — done

- OLED SSD1306 128×64 on I2C (SDA IO32, SCL IO33, 3V3 power), address 0x3C/0x3D detected automatically; SH1106 is not supported.
- Button on IO4 to GND (internal pull-up; IO4 does not affect boot). It acts on release: short — display/page; 5 s — one-time start in "Client" mode («Клієнт») with the setup access point (flag in RTC memory, the saved mode is not changed); 10 s — `nvs_flash_erase()` and restart. A button held at power-on is ignored until released.
- Pages: network, device / cable, traffic (client), script outputs (`output()`), system. The display goes dark after 60 s without presses.
- 6×10 font with Cyrillic — misc-fixed (public domain), table generated by `wt32/tools/gen_font.py`. Page and button logic is pure C with host tests; driver and task in `display.c`.
- **[to verify]** orientation and contrast on a real module; NVS erase while Wi-Fi is running.

## Scripts (Berry) — done

- Component `shared/script_berry` (Berry is a git submodule at a pinned commit). One script in the `storage` partition; editor and console on the page.
- Limits: 40 KB memory (budget allocator), 2 s per run (the VM heartbeat hook raises `timeout_error`), any error stops the script, a crash during a script turns autostart off.
- API: `print`, `output`, `every/after/cancel`, `status()`, `millis`, `heap`.
- Measurements (ESP-IDF 6.1 build, -Og): +~102 KB flash, +6.5 KB static RAM; at run time, 10 KB stack + the budget.
- `output()` values are also shown on the display (the "Script" page). Next: script access to the device over TCP — once the "shared IP" issue is solved (see the caveat above).

## Flash budget

Partition table `shared/partitions/4mb_ota.csv` (both firmwares): two OTA slots of 1.875 MB each with rollback, and 128 KB `storage` (littlefs) for scripts. The WT32 with Berry, DHCP and the display is ~1.16 MB → ~0.71 MB headroom.

## Updates (OTA)

Through the page: a `…-ota.bin` file, chip/project check before writing, write into the inactive slot, a 60 s "trial" after start, otherwise rollback to the previous firmware.

`/api/status` carries `build` (project, version, build date/time, source commit from `git describe --always --dirty` at configure time, ELF SHA-256, ESP-IDF), shown in the page's Firmware section. A release image is built from a clean git checkout so the commit is exact (a copy without `.git` gives `unknown`, local changes give `-dirty`).

## Page languages

- Texts are flat JSON files `i18n/<code>.json` next to each part that has page texts (`wt32/main`, `tools/c3-programmer/main`, `shared/wifi_setup`, `shared/script_berry`); `en` is the reference, `_name` / `_label` name the language. `portal_i18n()` (`shared/wifi_setup/project_include.cmake`) runs `tools/i18n_bundle.py` at build time: the folders of an app are merged, missing texts are filled from English, placeholders are checked, and the result is compiled in as `portal_langs[]`.
- The device serves `GET /i18n.json?l=<code>` (public: the login form needs it). `auth.js` holds the language: `data-i18n*` attributes in the HTML (the HTML itself is English, shown until the texts load), `I18N.t()` for texts built in JS, a switch in the header and on the login form, the choice in `localStorage`, the browser's language as the default.
- The device never sends display text: errors are `{"ok":false,"key":"err.…","message":"English"}` and the page shows the key's text in its language. So a new language needs no firmware code, only JSON files.
- The display uses the same files (keys `disp.*`, one 21-character line each, checked by the bundler; the 6×10 font has Latin and Cyrillic only): `disp_ui.c` reads the texts out of the language's bundled JSON. The language is the device's: saved in NVS (`wifi_setup`/`lang`) by `POST /api/lang` when it is switched on the page, and the default of a browser without its own choice.

## Tools

- `shared/wifi_setup/` — shared component: Wi-Fi station with settings in NVS, setup access point with captive portal, basic web API (`/api/status`, `/api/scan`, `/api/wifi`, `/api/forget`), page password (default `12345678`, changeable on the page; salt + SHA-256 in NVS; 30 min cookie sessions; 30 s lockout after 5 failed attempts) and `auth.js` with the login form. Mode `sta_netif = false` — a station without IP for the transparent bridge.
- `tools/c3-programmer/` — ESP32-C3 SuperMini as a WT32 programmer: RFC2217 over Wi-Fi + USB at a fixed 115200; Wi-Fi is configured through a captive portal, prebuilt image in `firmware/`. It uses the same `shared/wifi_setup` portal as the WT32 (access point `…-Setup-XXXX`, DNS interception, fallback after 60 s).

## Stages

The main firmware is `wt32/` (stages 0–2 are built into it; on hardware: the C3 programmer and the Client mode basics, the rest not yet — checklist with results in `wt32/README.md`).

0. ✅ C3 programmer, verified on hardware; ⏳ hardware check with a real device: Client mode basics done (checklist items 1, 3, 4, 10), the rest pending.
1. ✅ Mode switch ("Client" / "Router" / "Access point") and captive portal; page password; own DHCP server with address reservations.
2. ✅ Shared IP and management port.
3. ✅ Old code removed; OTA through the page with rollback.
4. ✅ Berry (see above).
5. ✅ Display and button.
6. ✅ Printable case (`hardware/case/`): WT32 from the STEP model, checks for intersections, clearances, walls and overhangs; ⏳ check display / USB-C / button against real parts, test print.
7. ✅ Page in English and Ukrainian (languages as JSON files); firmware info (version, build time, commit, hash) on the page.
