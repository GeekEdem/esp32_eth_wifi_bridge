# C3 programmer for the WT32-ETH01

*[Українська](README_UA.md)*

Firmware for the **ESP32-C3 SuperMini** that turns it into a programmer and serial monitor for the WT32-ETH01:

- **Over Wi-Fi (RFC2217):** `esptool` / `idf.py` flash the WT32 and read its log as an ordinary port `rfc2217://c3prog.local:4000`. Baud rate and DTR/RTS are passed over the network: RTS → EN, DTR → IO0, as on a regular USB-UART adapter with auto-reset.
- **Over USB (fallback):** C3 USB ↔ WT32 UART at a fixed 115200. The bootloader is entered with the button or from the web page.
- **Web page** `http://c3prog.local` (English / Ukrainian): Wi-Fi selection, status, **Restart** and **Download mode** buttons for the WT32, the C3's own firmware (version, build time, commit, SHA-256) and its update.

> Status: built (ESP-IDF 6.1). **Verified on hardware** (C3 firmware 0.3.1 and later): flashing the WT32 over Wi-Fi (`rfc2217://c3prog.local:4000`), its log over `miniterm`, `c3prog.local` over mDNS. The USB path, the BOOT button actions and the page's OTA are not verified yet.

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

Wi-Fi and the setup portal are the shared component [`shared/wifi_setup`](../../shared/wifi_setup). Dependencies (`rfc2217-server`, `mdns`) are pulled from GitHub. Pins, RFC2217 port, Wi-Fi TX power: `idf.py menuconfig` → *C3 Programmer Configuration*.

Page texts: [`main/i18n/<code>.json`](main/i18n/) plus the shared ones in [`shared/wifi_setup/i18n/`](../../shared/wifi_setup/i18n/); a new language is a new `<code>.json` in both folders (details in [`wt32/README.md`](../../wt32/README.md#languages)). Test: `shared/wifi_setup/test` (login and languages).

## Known limitations

- One RFC2217 network client at a time.
- Before 0.3.1 the first buffer purge from esptool/pyserial (`timeout while waiting for option 'purge'`) hung the connection thread, and the server then refused every new client until the C3 was restarted. Fixed in 0.3.1: flash the new `c3-programmer.bin` over USB (or `-ota.bin` through the page).
- If the laptop disappears mid-session (sleep, Wi-Fi drop), the connection may stay open until TCP gives up, and until then new connections are not accepted. Restart the C3 (BOOT 5–10 s) if that happens.
- The page is password-protected (default `12345678`), but the RFC2217 port itself (4000) has no password: anyone on the same network can flash or reset the WT32. Acceptable for a home bench.
- The setup access point is open (no Wi-Fi password); the page behind it is password-protected.
- Wi-Fi TX power is lowered to 8.5 dBm by default — the usual fix for SuperMini antenna problems. If the link is weak, change it in `menuconfig`.
- The C3's own log goes only to UART0 (GPIO21), because USB is given over to WT32 data.
