# WT32-ETH01: Wi-Fi for one Ethernet device

*[Українська](README_UA.md)*

Main firmware. The WT32 is connected by cable to **one** device (any device) and gives it Wi-Fi (or, in Access point mode, shares the router's network from the cable over Wi-Fi). The firmware knows nothing about the device and does not change its data — it only forwards frames.

> Status: built (ESP-IDF 6.1); the forwarding, management port and script logic is tested on a PC, the page in a browser against a mock API. **On hardware** (0.8.1 and earlier): Client mode with a device (link, device MAC/IP, ping, normal use, longer transfers, a WT32 restart, the page on the management port), the firmware update through the page with rollback confirmation, and Berry scripts (checklist items 1, 3–5, 8, 10–12, 27–34); see [Bench results](#bench-results-2026-09-30--2026-10-03). Router and Access point modes, the display and the button are not verified yet.

## Three modes (chosen on the page, applied after a restart)

### Client (default)
The WT32 connects to an existing Wi-Fi network. The device gets an IP from that network's router and is reachable as if it were plugged into the router with a cable.

- A Wi-Fi client can have only one MAC, so the device's frames leave with the **WT32 station MAC**, and the MACs inside ARP and DHCP (chaddr, option 61) are rewritten. The router sees one client — under the WT32 station MAC (printed in the log at startup). Reserve the IP for this MAC on the router.
- The WT32 has no IP of its own: it uses the device's IP and takes only new TCP connections to port **28480** and connections it opened itself. Broadcast, multicast and ARP go to both; everything else (the device's services on any port, its web page on 80, ping) goes to the device. Netmask, gateway and DNS come from the router's DHCP reply to the device.
- Which address is the device's: the one from its DHCP lease, as soon as the router's reply passes by. A device with a static address: the address it sends from; if it uses several, the WT32 stays on the first one and moves only after it has been silent for 30 s while the device uses another. The page shows "(DHCP)" or "(static)" next to it.
- Page: **`http://<device IP>:28480`**. While the network is not configured or has been unreachable for 60 s, or the device's IP is not known yet, the page is also available through the **`WT32-Setup-XXXX`** access point → http://192.168.4.1 (open, no Wi-Fi password).
- A **static address from another network** (e.g. a printer set to `192.168.1.77` in a `192.168.50.x` network): nobody on the Wi-Fi network can reach it, the page included. The WT32 notices it — no DHCP, and no other host of that subnet heard on Wi-Fi within 12 s of joining the network (0.8.1, verified: no warning on boot, the access point goes off 4.7 s after DHCP; on 0.8.0 the warning also flashed for a second on every boot) — keeps the `WT32-Setup-XXXX` access point on, and the page and the display point there. The access point goes off once a host of the device's subnet is heard (or the device takes a DHCP address).
- A device that keeps its lease across a WT32 restart (it does not redo DHCP on link-up, like the test printer) shows "(static)" until its next DHCP renewal. That is expected: the WT32 only learns a lease from the router's reply.
- Lease-first addressing (0.7.2) is verified on hardware: the page shows "(DHCP)" once the device takes a lease, and the address no longer flaps. On 0.7.1 a dock that briefly sent from two IPv4 addresses after a re-lease made the management address flip every 1–3 s.
- The test printer came with a static address from another network; before 0.7.2 that left the WT32 unreachable. The 0.7.2 behaviour for this case (the setup access point stays on) has not been re-tested on hardware.
- IPv6 is not forwarded (MACs in neighbor discovery packets are not rewritten).

### Router
The WT32 runs its own Wi-Fi network (WPA2/WPA3). The access point and Ethernet are one network with a DHCP server on the WT32: the device on the cable and phones get addresses and see each other. There is no internet (NAT is not needed: there is nowhere to go).

- WT32 — `192.168.77.1` (configurable), DHCP pool — `.100–.200`; page — `http://192.168.77.1` or `http://wt32.local`.
- The device's IP is shown on the page (the WT32 remembers its MAC from the first frame and looks up the address it was given).
- The DHCP server is our own (`dhcp_core.c`): 2 h leases, up to 32 clients; a client gets the same address for its MAC. The **Network addresses (DHCP)** section of the page lists the clients (the device on the cable is marked) and **IP reservations by MAC** (with a button in the client's row or manually; any address in the subnet except the WT32's). Leases and reservations are stored in NVS and survive a restart; reservations are saved immediately, leases every 30 s.
- DHCP hands out the WT32's address as the gateway and no DNS (there is no internet).

### Access point
The WT32's cable goes into a router (or a switch on a network with DHCP), and the WT32 shares that network over Wi-Fi. The access point and Ethernet are one bridge: addresses (DHCP), internet, device discovery — all come from the router; the WT32 changes nothing.

- The WT32 is one more DHCP client on that network (hostname `wt32`); page — `http://<WT32 address>` (shown in the router's client list and on UART) or `http://wt32.local`.
- **No DHCP on the cable for 30 s** (cable unplugged, router rebooting) → the WT32 takes a fallback address (the **Fallback WT32 address** field, default `192.168.77.1`) and itself hands out `.100–.200` **only to clients of its own access point**, with 2-min leases, so the page stays reachable. Meanwhile, every 30 s and when the cable is plugged in, the WT32 sends a probe DHCP request; as soon as the router answers, it returns to the router's DHCP, and clients move to the router's addresses within 1–2 min.
- Wi-Fi name, password and channel are shared with Router mode.

## Web page

Password-protected: default **`12345678`**, changed in the **Page password** section (8–63 characters); while the default is set, the page shows a reminder. A session lasts until 30 min of inactivity; 5 failed attempts in a row lock login for 30 s.

The page is in **English and Ukrainian**: the EN / УКР switch is in the page header and on the login form. The choice is remembered in the browser and saved on the WT32, whose display uses it (a switch made before logging in is saved right after it; the first login saves the page's language if none is saved yet). A browser without its own choice opens the page in the WT32's language, or, while none is saved, in the browser's. Another language is one JSON file (see [Languages](#languages)).

- **Status**: mode, Wi-Fi, Ethernet, device MAC/IP, page address; in Client mode — traffic counters with explanations.
- **Mode**: the Client / Router / Access point switch; for Client — Wi-Fi network selection and password; for Router and Access point — name, password (8–63), channel, WT32 address (for Access point — the fallback one).
- **Forget the device (after replacing it)** — after replacing the device on the cable (Client). **Forget client Wi-Fi** — returns to the setup access point.
- **Firmware**: which firmware is running — version, build date and time, source commit, SHA-256 of the image, ESP-IDF version — and the update (below).

**Forgot the page password or the WT32 network password** → hold the button for 10 s (see below). Without a button — erase the flash and flash again (this erases all settings):
```bash
esptool.py --chip esp32 -p rfc2217://c3prog.local:4000 erase_flash
esptool.py --chip esp32 -p rfc2217://c3prog.local:4000 write_flash 0x0 firmware/wt32-bridge.bin
```

## Display and button (optional)

A 0.96″ 128×64 I2C OLED (SSD1306) and one button. Everything works without them; the button also works without the display. The display speaks the language chosen on the page (EN / УКР: the choice is saved on the WT32).

| What | WT32 pin |
|---|---|
| OLED VCC | **3V3** (not 5 V: the I2C pull-ups on the module go to its supply) |
| OLED GND | GND |
| OLED SDA | IO32 (labelled CFG on the board) |
| OLED SCL | IO33 (labelled 485_EN on the board) |
| Button | between **IO4** and GND (internal pull-up, no resistor needed) |

- The display is detected automatically at address 0x3C or 0x3D (in the log: `SSD1306 at 0x3C`). It turns off after 60 s without a press (an OLED burns in from a static image).
- Pages: **Network** (mode, Wi-Fi, WT32 page address), **Device** (Ethernet with the negotiated speed and duplex, e.g. `Ethernet: 100M full`, MAC, IP; in Access point mode — **Cable**: Ethernet, router DHCP, gateway), **Traffic** (Client), **Script** (the script's first 5 `output()` values, if any), **System** (version, memory, uptime).
- **Button** (acts on release; while you hold it, the display shows what will happen):
  - short — turn the display on / next page;
  - **5 s** — one-time setup start: the WT32 restarts in Client mode with the `WT32-Setup-XXXX` access point (http://192.168.4.1). The saved mode is not changed: after the next restart the WT32 returns to it, unless something else has been saved on the page;
  - **10 s** — reset: erases all settings (mode, networks, page password → `12345678`, DHCP leases, script autostart); the script text is kept;
  - 1–5 s — nothing (cancel). A button held down at power-on is ignored until it is released.

## Scripts (Berry)

The **Script (Berry)** section of the page: an editor, **Save and run**, **Stop**, **Run at startup**, the **Results** table and a console. A script can read the device status (`status()`), output results (`output()`) and run on timers (`every`, `after`). API and limits — [`../shared/script_berry/README.md`](../shared/script_berry/README.md).

- Script memory budget is 40 KB, a single run of code is limited to 2 s; an error stops only the script.
- "Free on the device" in the Script section is the same figure as the script's `heap()` (since 0.8.1, verified: 123.2 KB on the page against 123 KB from `heap()`; before, the page read it during its own request, 10–15 KB lower), followed by the lowest free memory since start.
- If the device restarted because of a crash while the script was running, autostart is turned off.
- Cost in the firmware: ~102 KB of flash, ~6.5 KB of static RAM; while running, another 10 KB of task stack plus the script budget.

## Firmware update from the page (OTA)

The **Firmware** section of the page: file **`firmware/wt32-bridge-ota.bin`** → **Update firmware**. The page rejects the full image `wt32-bridge.bin` (for address `0x0`) — it is only for flashing over the cable.

- Before writing, the image is checked to be for this chip and this firmware; after writing, its integrity is checked.
- The flash has two slots (`ota_0`/`ota_1`, 1.875 MB each); the new firmware is written to the inactive one, the old one stays.
- After the restart the new firmware is **on probation for 60 s**: if it crashes or restarts during that time, the bootloader goes back to the previous one. The Firmware table shows which version is actually running, with its build time and commit.
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
2. Client mode: choose the home network, enter its password → **Save and restart**.
   Or Router / Access point: name, password, channel → **Save and restart**.
3. Change the page password. The EN / УКР switch in the header changes the page language.

## 3. Checklist with a device

Any network device in **DHCP** mode (a printer is used for testing). A patch cord between the device and the WT32. "Normal use" means whatever the device is on the network for (a printer — printing from a PC, a camera — video, etc.).

Result: ✅ verified on hardware, with the WT32 firmware version it was verified on; empty — not verified yet.

**Client mode**

| # | Action | Expected | Result |
|---|---|---|---|
| 1 | Turn the device on | Ethernet — link up, the device MAC appears, then its IP | ✅ 0.7.0 |
| 2 | Router's client list | a new client with the **WT32 station MAC** and the IP shown on the page | |
| 3 | From a PC: `ping <IP>` | replies | ✅ 0.7.0; 0.8.0: 4–9 ms |
| 4 | Normal use of the device at `<IP>` | works | ✅ 0.7.0 |
| 5 | Longer transfer (large file, several jobs in a row) | works without interruptions | ✅ 0.8.0 (a PC as the device): 20 s each way, 6.8 / 7.8 Mbit/s, 0 % ping loss (see Bench results) |
| 6 | Vendor utility → search for the device on the network | finds it / doesn't (write down) | |
| 7 | Turn the device off and on | same IP (only with a DHCP reservation for the WT32 station MAC on the router), normal use works | not met on 0.8.0: the router gave the printer a different address after a power cycle (no reservation) |
| 8 | Restart the WT32 (device stays on) | connection comes back without restarting the device | ✅ 0.7.2, 0.8.0: link back in 3.3 s, the device redoes DHCP, router reachable at 5.1 s |
| 9 | Restart the router | connection comes back by itself | |
| 10 | From a PC open `http://<IP>:28480`, log in | WT32 page; the **WT32 management traffic** counter grows; the `WT32-Setup` access point is gone | ✅ 0.7.1 |
| 11 | Use the device (item 4) with the page from item 10 open | works, the page keeps updating | ✅ 0.8.0: the page kept updating through a 30 s 7.5 Mbit/s transfer |
| 12 | If the device has a web page: `http://<IP>` | the **device's** page opens, not the WT32's | ✅ 0.8.0: the printer's own page on :80; the WT32 page only on :28480 |
| 13 | Change the page password, log out, log in with the new one | works; the old password is rejected | |

**Router mode**

| # | Action | Expected | Result |
|---|---|---|---|
| 14 | Switch to Router, save | WT32 restarts with the new network | |
| 15 | Connect a phone to it, `http://192.168.77.1` | WT32 page, mode "router" | |
| 16 | Page → device IP | an address from `.100–.200` | |
| 17 | Laptop on the same network: normal use of the device at `<IP>` | works | |
| 18 | **Network addresses (DHCP)**: **Pin** in the device's row | pinned | |
| 19 | Restart the WT32 and the device | the device gets the same address, the reservation is still there | |
| 20 | **Pin manually**: device MAC + `192.168.77.50`, restart the device | the device is on `.50` | |

**Access point mode** (the WT32's cable goes into a LAN port of the home router, not into the device)

| # | Action | Expected | Result |
|---|---|---|---|
| 21 | Page → Access point, save; cable into the router | WT32 restarts, network with the same name | |
| 22 | Connect a phone to the WT32 network | address from the router (as at home), internet works | |
| 23 | `http://wt32.local` or the WT32 address from the router's client list | page: "WT32 address … (from the router)", gateway — the router | |
| 24 | Unplug the cable, after 1 min reconnect the phone to the WT32 network | the phone gets `192.168.77.1xx`; page at `http://192.168.77.1`, "fallback: no DHCP on the cable" | |
| 25 | Plug the cable back in | within ≤ 30 s the WT32 has an address from the router again; the phone within 1–2 min (or reconnect it) | |
| 26 | Switch back to Client | connects to the home Wi-Fi | |

**Update**

| # | Action | Expected | Result |
|---|---|---|---|
| 27 | Page → Firmware → `wt32-bridge.bin` | rejected: "This is not an update file. Use the "…-ota.bin" image…" | ✅ 0.8.0 |
| 28 | Same with `wt32-bridge-ota.bin` | progress, restart, login, "Updated to version…", the partition has changed (`ota_0` ↔ `ota_1`); the Firmware table shows the new version, build time and commit | ✅ 0.8.0: ota_0 → ota_1, 1.2 MB in ~20 s; the Firmware table shows version, build time, commit, partition and the 60 s note |
| 29 | 1–2 min after item 28, restart the WT32 (power off) | starts from the new firmware (it has been confirmed) | ✅ 0.8.0: "ota: new firmware confirmed after 60 s"; a reset (EN through the programmer, not a power cut) boots ota_1 again |

**Scripts**

| # | Action | Expected | Result |
|---|---|---|---|
| 30 | Script (Berry) → example → Save and run | state "running"; Results shows mode, Ethernet, device IP, memory; updates every 5 s | ✅ 0.8.0 |
| 31 | Use the device while the script runs | works as usual | ✅ 0.8.0: 5.9 Mbit/s while the example ran |
| 32 | Script `while true end` | "error — timeout_error…"; the page and the device keep working | ✅ 0.8.0: "timeout_error: script code ran too long"; page and bridge kept working |
| 33 | Turn on Run at startup, restart the WT32 | the script starts by itself | ✅ 0.8.0: also after the OTA restart |
| 34 | UART log, `script` line / **Script memory** on the page | write down the device's free memory with the example running | ✅ 0.8.0: script memory 7.8–8.3 KB (peak 12.8 KB) of 40 KB; free on the device 108–116 KB on the page, 123–124 KB from `heap()`; `script:` lines on UART |

**Display and button** (wiring — above)

| # | Action | Expected | Result |
|---|---|---|---|
| 35 | Power on the WT32 with the display | log `SSD1306 at 0x3C` (or 0x3D); the display shows the Network page, text is readable, not mirrored | |
| 36 | Press the button briefly, repeatedly | pages cycle, the number is in the title | |
| 37 | Leave it for 1 min; then press | the display turns off; the first press only turns it on | |
| 38 | Hold for 3 s and release | the display shows "Hold until 5 s:" and a bar; nothing happens after release | |
| 39 | Hold for 6 s and release | restart; `WT32-Setup-XXXX` access point, the page shows "One-time setup start"; another restart — the saved mode | |
| 40 | Hold for 11 s and release | "RESET the settings", restart; Client mode with no network, page password `12345678` | |
| 41 | On the page switch EN ↔ УКР; then restart the WT32 | the display changes language within a second; after the restart it keeps the last choice | page part only, 0.8.0: the language survives a restart; the display part is not verified |

### Bench results (2026-09-30 … 2026-10-03)

WT32-ETH01 (ESP32-D0WD rev 1.0, 4 MB) wired to the C3 SuperMini programmer as in [`docs/WIRING.md`](../docs/WIRING.md); flashed and logged over RFC2217 with stock esptool 4.8.1 / pyserial 3.5 on Windows 11. Final state: WT32 0.8.0 running from `ota_1` after a page OTA, C3 0.4.1; re-tested on WT32 0.8.1 and C3 0.4.2 (last bullet). Devices on the cable at different times: a receipt-style network printer (web page on :80, 100 Mbit/s full duplex, no PAUSE, does not redo DHCP on a link bounce), a USB-C dock with a Linux laptop, and the PC's own Ethernet (negotiates PAUSE).

- **Throughput (item 5, the PC as the device):** traffic pushed through the bridge by binding source addresses (client on the PC's Ethernet address, server on its Wi-Fi address), 2.4 GHz, RSSI −67…−74 dBm. 20 s each way without interruptions: 6.8 Mbit/s device → Wi-Fi, 7.8 Mbit/s Wi-Fi → device, 0 % ping loss. Ping under the Wi-Fi → device load ~120 ms on average (max 182 ms). Counters after all runs: "Waited for Wi-Fi" 194, Wi-Fi transmit errors 40 (last `ESP_ERR_NO_MEM`) of ~80 k device → Wi-Fi frames. For comparison, on 0.6.0 about 14 % of the frames failed under a ~15 Mbit/s download, with ping at 200–330 ms.
- **Ping** to the device: 4–9 ms (0.8.0).
- **WT32 restart with the device on (item 8):** link back at 3.3 s, the device repeats DHCP, the router is reachable through the bridge at 5.1 s.
- **Device power cycle (item 7):** the router handed the printer a different address; a DHCP renewal from the PC kept the same one. Keeping the address needs a reservation for the WT32 station MAC.
- **Memory (item 34):** with the example script running, script memory 7.8–8.3 KB (peak 12.8 KB) of 40 KB; free on the device 108–116 KB as shown in the Script section, 123–124 KB from the script's `heap()` (the two figures differ; see 0.8.1).
- **Power:** see the power note in [`docs/WIRING.md`](../docs/WIRING.md): powered from the programmer's 5 V, the WT32 brownout-reset in a loop as soon as Wi-Fi started with the Ethernet link up.
- **Re-test on WT32 0.8.1 and C3 0.4.2 (2026-10-03):** no false "another network" warning on boot, the setup access point goes off 4.7 s after DHCP; "Free on the device" 123.2 KB against 123 KB from `heap()`, lowest since start 97.4 KB; the script console under 40 s of 7.9 Mbit/s with two stop/start cycles showed no repeated lines (start #1..#3); WT32 management frames under that load: 2 transmit errors (last `ESP_ERR_NO_MEM`, waited 4), against 114 on 0.8.0. C3 0.4.2 with the RFC2217 connection routed through the WT32 on purpose (routing pitfall, [`docs/WIRING.md`](../docs/WIRING.md)): the EN watchdog released EN, the WT32 and its link were back in 6–10 s, but a new RFC2217 client got in only after 31–34 s (cause found and fixed in C3 0.4.5: 15–17 s, see the C3 README).
- **Not tested on hardware yet:** items 2, 6, 9, 13; Router and Access point modes (14–26); the display and button (35–40; 41 only on the page).

If something does not work, attach the filled-in tables, screenshots of the page and a few lines of the log to an issue.

**Hints on the Traffic page** (Client):
- "Dropped: Wi-Fi not connected" grows — the WT32 cannot hold Wi-Fi (check RSSI);
- "Frames from a second device" > 0 — more than one device on Ethernet (a switch?) — only one is supported;
- "DHCP rewrites" = 0 and no IP — the device does not send DHCP (a static IP from another subnet?);
- "Waited for Wi-Fi" grows under load — normal: the device sends faster than Wi-Fi carries, so the WT32 holds its frames (up to 50 ms each) and, if the device supports it, slows it down with Ethernet PAUSE frames (flow control, see the `eth: link up …` line in the log) instead of dropping them;
- "Transmit errors Wi-Fi / Ethernet" grow — frames dropped after all; the UART summary gives the reason (`last ESP_ERR_NO_MEM` = Wi-Fi was still full after 50 ms). With flow control off (the device does not take PAUSE frames), some loss under a sustained flood is expected; otherwise open an issue with the UART log.

**Ethernet LEDs**: the LAN8720 drives them by itself, the firmware cannot. On the WT32-ETH01 one blinks with link and traffic; the other is most likely wired to the LAN8720's speed output (LED2), which lights at 100 Mbit/s only. The log line `eth: link up: 100 Mbit/s, full duplex, flow control on` and the Ethernet row on the page show what was negotiated: if they say 100 Mbit/s and that LED stays dark, it is the board, not the firmware; if they say 10 Mbit/s or half duplex, try another cable (or the device is 10 Mbit/s only).

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
# language files: valid, complete, and every key the page and the firmware use exists
python3 ../../shared/wifi_setup/tools/i18n_bundle.py check --complete ../main/i18n ../../shared/wifi_setup/i18n ../../shared/script_berry/i18n \
  --sources ../main/page.html ../main/*.c ../../shared/wifi_setup/*.js ../../shared/wifi_setup/*.c ../../shared/script_berry/script.js ../../shared/script_berry/*.c
# page (needs playwright): login and modes; update — rejection / update / rollback; script; DHCP; languages.
# The mock keeps state: start a fresh one for each test.
python3 mock_wt32.py ../main/page.html ../../shared/wifi_setup/auth.js 8811 &
node mode_flow.js 8811          # also: ota_flow.js 8811 ../firmware/wt32-bridge.bin ../firmware/wt32-bridge-ota.bin,
kill %1                         #       script_flow.js 8811, dhcp_flow.js 8811, ../../shared/wifi_setup/test/i18n_flow.js 8811 wt32
```

## Languages

The page texts live in JSON files, one per language: [`main/i18n/`](main/i18n/) (this page, and the display: keys `disp.*`), [`shared/wifi_setup/i18n/`](../shared/wifi_setup/i18n/) (login, password, firmware, device errors) and [`shared/script_berry/i18n/`](../shared/script_berry/i18n/) (scripts). The build merges them into the firmware (`portal_i18n()` in `main/CMakeLists.txt`), and the device serves them at `/i18n.json`.

To add a language, e.g. German: copy `en.json` to `de.json` in each of the three folders, translate the values (keep the keys and the `{placeholders}`), set `"_name": "Deutsch"` and `"_label": "DE"` in the `shared/wifi_setup/i18n/de.json`, and rebuild. The switch lists the new language by itself (as a drop-down from four languages on). A text missing in a language is shown in English; `i18n_bundle.py check` (above) lists what is missing. The display texts (`disp.*`) must fit a 21-character line (the check says which do not), and the display font has Latin and Cyrillic letters only: other scripts show as `?` there.

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
| `main/i18n/<code>.json` | page texts per language (see [Languages](#languages)) |
