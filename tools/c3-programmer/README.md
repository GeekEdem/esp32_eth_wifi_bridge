# C3 programmer for the WT32-ETH01

*[Українська](README_UA.md)*

Firmware for the **ESP32-C3 SuperMini** that turns it into a programmer and serial monitor for the WT32-ETH01:

- **Over Wi-Fi (RFC2217):** `esptool` / `idf.py` flash the WT32 and read its log as an ordinary port `rfc2217://c3prog.local:4000`. Baud rate and DTR/RTS are passed over the network: RTS → EN, DTR → IO0, as on a regular USB-UART adapter with auto-reset.
- **Over USB (fallback):** C3 USB ↔ WT32 UART at a fixed 115200. The bootloader is entered with the button or from the web page.
- **Web page** `http://c3prog.local` (English / Ukrainian): Wi-Fi selection, status, **Restart** and **Download mode** buttons for the WT32, the C3's own firmware (version, build time, commit, SHA-256) and its update.

> Status: built (ESP-IDF 6.1). **Verified on hardware** (C3 0.3.1–0.4.6, stock esptool 4.8.1 / pyserial 3.5 on Windows 11): `flash_id`, `read_flash` and `write_flash` over RFC2217 (at 460800 about 300 kbit/s effective: 1.2 MB in ~31 s), the log over `miniterm`, RTS → EN resets, repeated reconnects with buffer purges, `c3prog.local` over mDNS, the C3's page; on 0.4.2 the EN watchdog (the WT32 back in 6–10 s after a cut connection); on 0.4.3 a monitor that only reads stays connected (51 s without commands); on 0.4.5 the next client 15–17 s after the routing pitfall, and a second client waits in the queue without disturbing the first; on 0.4.5 the USB fallback path (the WT32's log on the C3's USB port; with the WT32 put into its ROM bootloader over RFC2217, esptool over USB with `--before no_reset --after no_reset` at 115200: `flash_id`, `read_mac`). On 0.4.5 the BOOT button: < 1 s put the WT32 into its ROM bootloader, 1–5 s restarted it, 5–10 s restarted the C3 without touching the WT32, ≥ 10 s forgot the Wi-Fi (the setup access point came up; the page password reset was not checked separately). On 0.4.6 the C3's own update through its page: 0.4.5 (ota_0) → 0.4.6 (ota_1), confirmed after 60 s, the page shows the new version and commit.

## 1. Flash the C3 with the prebuilt binary

File: [`firmware/c3-programmer.bin`](firmware/c3-programmer.bin) — a single image, written at address `0x0`.

**From the browser (Chrome/Edge, nothing to install):** https://espressif.github.io/esptool-js/ → *Connect* → select the C3 port → *Flash Address* `0x0`, file `c3-programmer.bin` → *Program*.

**Or with esptool:**
```bash
pip install esptool
esptool.py --chip esp32c3 -p /dev/ttyACM0 write_flash 0x0 c3-programmer.bin    # Windows: -p COM5
```

If the C3 does not show up as a port: hold BOOT, press RESET, release BOOT, and try again.

## 2. Connect to Wi-Fi

1. After flashing, the C3 starts the access point **`C3prog-Setup-XXXX`** (open). The LED double-blinks.
2. Connect to it with a phone — the setup page opens by itself (if not, go to http://192.168.4.1). The page password is `12345678`; change it in the **Page password** section. The EN / УКР switch (on the login form and in the header) changes the page language.
3. Choose the network, enter its password, optionally change the device name → **Save and restart**.
4. The C3 connects to the network; the page is at `http://c3prog.local` (or the IP from the router). The LED stays on.

If the saved network is unavailable for 60 s, the setup access point is turned on again (the C3 keeps trying to connect meanwhile).

## Updating the C3 firmware through the page (OTA)

In the **Firmware** section of the page: file **`firmware/c3-programmer-ota.bin`** → **Update firmware**. The page rejects the full image `c3-programmer.bin` (for address `0x0`) — it is only for flashing over the cable.

- Before writing, the image is checked to be for this chip and this firmware; after writing, its integrity is checked.
- The flash has two slots (`ota_0`/`ota_1`, 1.875 MB each); the new firmware is written into the inactive one, the old one stays.
- After the restart the new firmware is **on trial for 60 s**: if it crashes or restarts within that time, the bootloader returns to the previous one. The Firmware table shows which version is actually running, with its build time and commit.
- Sessions do not survive a restart — the page asks you to log in again.
- The first time, firmware with this partition layout must be written over the cable (full image at `0x0`); the settings (NVS) are kept.

## Wiring to the WT32

Diagram and first-flash steps: [`docs/WIRING.md`](../../docs/WIRING.md).

| C3 SuperMini | WT32-ETH01 | Note |
|---|---|---|
| GPIO4 (UART1 TX) | RXD (IO3) | |
| GPIO5 (UART1 RX) | TXD (IO1) | internal pull-up on the C3 |
| GPIO6 | EN | open-drain: only pulls low or releases |
| GPIO7 | IO0 | open-drain; always released after the WT32 starts (IO0 = Ethernet clock) |
| 5V | 5V | bench use only; **not 3V3** |
| GND | GND | required |

## BOOT button and LED on the C3

| Hold | Action |
|---|---|
| < 1 s | WT32 → flash mode |
| 1–5 s | restart the WT32 |
| 5–10 s | restart the C3 itself |
| ≥ 10 s | forget Wi-Fi and reset the page password to `12345678` (the C3 returns to setup mode) |

A button already down when the C3 starts counts only from its release (0.4.6; on hardware the WT32 kept running through a hold past 10 s). Up to 0.4.5 a hold past 10 s restarted the C3 with the button still down, and the new boot took the rest of the hold as a new press: on release it restarted the WT32 (1–5 s) or put it into flash mode (< 1 s); seen on hardware, the WT32 restarted 2 s after the C3.

LED: double blink — setup access point; 1 Hz — connecting to Wi-Fi; steady on — ready; flickering — data exchange with the WT32.

## Flashing the WT32 over Wi-Fi

```bash
esptool.py --chip esp32 -p rfc2217://c3prog.local:4000 flash_id
idf.py -p rfc2217://c3prog.local:4000 flash monitor
```

Monitor only (without `idf.py`):
```bash
python -m serial.tools.miniterm --rts 0 --dtr 0 rfc2217://c3prog.local:4000 115200
```
`--rts 0 --dtr 0` is required: by default pyserial raises RTS, and the WT32 stays in reset.

If `.local` does not resolve (common on Windows without Bonjour), use the IP from the status page.

If auto-reset over the network does not work: press **Download mode** on the page (or BOOT briefly on the C3), then run `esptool.py ... --before no_reset ...`.

## Flashing the WT32 over USB (fallback)

The C3's USB does not pass the baud rate or DTR/RTS, so:
- only `-b 115200`;
- `--before no_reset --after no_reset`: reset signals from the PC would reset the C3 itself, not the WT32.

```bash
# 1. "Download mode" on the page or BOOT briefly on the C3
cd build   # WT32 firmware build directory
esptool.py --chip esp32 -p /dev/ttyACM0 -b 115200 --before no_reset --after no_reset write_flash @flash_args
# 2. "Restart" on the page or BOOT 1–5 s
```

Monitor: `python -m serial.tools.miniterm --dtr 0 --rts 0 /dev/ttyACM0 115200`
(`--dtr 0 --rts 0` so as not to reset the C3).

## Building from source

```bash
. $IDF_PATH/export.sh            # ESP-IDF ≥ 5.1, tested on 6.1
cd tools/c3-programmer
./build_firmware.sh              # → firmware/c3-programmer.bin (cable, 0x0) and c3-programmer-ota.bin (page)
# or: idf.py set-target esp32c3 && idf.py build flash
```

Wi-Fi and the setup portal are the shared component [`shared/wifi_setup`](../../shared/wifi_setup). `mdns` is pulled from GitHub; `rfc2217-server` v0.4.0 is kept in [`components/rfc2217-server`](components/rfc2217-server) with a patch that adds TCP keepalive (see its `PATCHES.md`). Pins, RFC2217 port, Wi-Fi TX power: `idf.py menuconfig` → *C3 Programmer Configuration*.

Tests on a PC: `test/client_watch_test.c` (when to probe and drop; `cc -Wall -Wextra -fsanitize=address,undefined -Imain -o /tmp/cwt test/client_watch_test.c main/client_watch.c && /tmp/cwt`) and `test/rfc2217_lwip/` (the server on lwIP from ESP-IDF over a tap interface, the routing pitfall with iptables, needs root and pyserial: `build.sh /tmp/h && sudo python3 run.py /tmp/h`; `run.py --half-open` adds a connection attempt given up before the handshake and left half-open in lwIP, as on the bench with 0.4.2–0.4.4: with a listen backlog of 1 the next client waits ~20 s, since 0.4.5 ~17 s; runs in WSL2 too).

Page texts: [`main/i18n/<code>.json`](main/i18n/) plus the shared ones in [`shared/wifi_setup/i18n/`](../../shared/wifi_setup/i18n/); a new language is a new `<code>.json` in both folders (details in [`wt32/README.md`](../../wt32/README.md#languages)). Test: `shared/wifi_setup/test` (login and languages).

## Known limitations

- One RFC2217 network client at a time. Since 0.4.5 up to four more connections wait in a queue; a waiting esptool/pyserial gives up after ~3 s with "Remote does not seem to support RFC2217 or BINARY mode": run it again once the current client is gone.
- Before 0.3.1 the first buffer purge from esptool/pyserial (`timeout while waiting for option 'purge'`) hung the connection thread, and the server then refused every new client until the C3 was restarted. Fixed in 0.3.1: flash the new `c3-programmer.bin` over USB (or `-ota.bin` through the page).
- If the laptop disappears mid-session (sleep, Wi-Fi drop), the C3 notices it, drops the connection, releases EN and IO0 and accepts the next client. Since 0.4.3, after 10 s without anything from the client the C3 asks it for an answer (a telnet option request that pyserial rejects at once) and drops it if none comes within 5 s (`menuconfig` → *Drop an RFC2217 client…*); a monitor that only reads answers and stays (verified on 0.4.3). Since 0.4.4 the drop is a hard close (RST) made by its own task. 0.4.2 relied on TCP keepalive alone (also ~15 s on paper). On hardware (debug build log, 0.4.4) the dead client of the routing pitfall below is gone ~15 s after its last packet (TCP keepalive aborts it first). Before 0.4.2 the connection stayed open until TCP gave up, no new client was accepted, and if EN (RTS) was asserted at that moment the WT32 stayed in reset until the C3 was restarted (BOOT 5–10 s).
- Since 0.4.2, EN held by a client for more than 3 s with no further control request is released together with IO0, and the log says so (`menuconfig` → *Release EN…*; esptool and idf.py hold it ~100 ms). A side effect: miniterm started without `--rts 0` no longer keeps the WT32 in reset for more than 3 s. Verified on 0.4.2 (routing pitfall below): EN released, the WT32 and its link back in 6–10 s.
- **Routing pitfall:** if the PC that runs esptool/miniterm also has a wired interface behind the WT32 (e.g. testing with the PC as the device), the OS may route the RFC2217 connection through the WT32 (on Windows, Ethernet metric 5 is below Wi-Fi's 35). An RTS reset then cuts the connection that carries it. On 0.4.0 and 0.4.1 the WT32 then stayed in reset and the C3 accepted no new client until it was restarted (BOOT 5–10 s, or a USB reset). Since 0.4.2 the EN watchdog brings the WT32 back in 6–10 s, and the dead client is dropped after ~15 s. On 0.4.2–0.4.4 a new client still got in only after 31–34 s, or after more than a minute when retried every few seconds: with a listen backlog of 1, a retry that gave up while the server was busy left a half-open connection in the only slot (SYN-RCVD: its socket was closed when the C3's SYN-ACK came, and Windows' firewall sends no RST), lwIP kept it ~18 s and refused everyone else, and the next retry did the same (seen in lwIP's TCP table in a debug build; `clientFor`, `clientDrops` and `clientDroppedAgo` in `/api/status` tell the same on a normal build). Since 0.4.5 the backlog is 4: verified on hardware, the next client gets in 15–17 s after the reset (three runs, retries every 2.7–4 s), a second client while one is connected waits and gets the message above, and the first is not disturbed. To avoid the pitfall altogether, bind the client to the other interface or add a host route to the C3.
- The page is password-protected (default `12345678`), but the RFC2217 port itself (4000) has no password: anyone on the same network can flash or reset the WT32. Acceptable for a home bench.
- The setup access point is open (no Wi-Fi password); the page behind it is password-protected.
- Wi-Fi TX power is lowered to 8.5 dBm by default — the usual fix for SuperMini antenna problems. If the link is weak, change it in `menuconfig`.
- The C3's own log goes only to UART0 (GPIO21), because USB is given over to WT32 data.
