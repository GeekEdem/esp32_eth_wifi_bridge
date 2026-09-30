# WT32-ETH01: Wi-Fi for an Ethernet-only device

*[Українська](README_UA.md)*

Firmware for the **WT32-ETH01** (ESP32 + LAN8720) that gives Wi-Fi to a device that only has an Ethernet port (any device: a printer, a controller, a NAS, …), or shares the network on its cable over Wi-Fi. The firmware knows nothing about the device and never changes its data: it only forwards frames.

> Status: everything builds and passes the tests on a PC; **it has not been verified on hardware yet**.

## Modes

| Mode | How it works | WT32 page |
|---|---|---|
| **Client** (default) | The WT32 joins an existing Wi-Fi; the device gets its IP from that network's router and is reachable as if it were plugged into it | `http://<device IP>:28480` |
| **Router** | The WT32 runs its own Wi-Fi with DHCP; the device and phones share one network, without internet | `http://192.168.77.1` |
| **Access point** | The WT32's cable goes into a router; the WT32 shares that network over Wi-Fi (DHCP and internet come from the router) | `http://wt32.local` or the address given by the router |

First setup goes through the `WT32-Setup-XXXX` access point. The page is password-protected (default `12345678`).

The web page and the display are currently in Ukrainian.

## Layout

| Folder | What it is |
|---|---|
| [`wt32/`](wt32/) | the main WT32-ETH01 firmware; prebuilt image in `wt32/firmware/`, instructions and the hardware checklist in [`wt32/README.md`](wt32/README.md) |
| [`tools/c3-programmer/`](tools/c3-programmer/) | ESP32-C3 SuperMini as a programmer and serial monitor for the WT32, over Wi-Fi (RFC2217) or USB |
| [`shared/wifi_setup/`](shared/wifi_setup/) | shared component: Wi-Fi with settings in NVS, setup access point with a captive portal, web API, page password, OTA |
| [`shared/script_berry/`](shared/script_berry/) | user scripts in Berry: editor on the page, memory and time limits |
| [`hardware/case/`](hardware/case/) | 3D-printable case (STL + a Python generator checked against the board's 3D model): WT32, display, button, USB-C power |
| [`docs/WIRING.md`](docs/WIRING.md) | wiring diagrams: programmer → WT32, WT32 with power, display and button; first flash |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | decisions, stages, open questions |

## Quick start

1. Flash the C3 with `tools/c3-programmer/firmware/c3-programmer.bin` (at address `0x0`) and connect it to Wi-Fi through `C3prog-Setup-XXXX`.
2. Wire the C3 to the WT32 (diagrams and the first-flash steps: [`docs/WIRING.md`](docs/WIRING.md)) and flash the WT32:
   ```bash
   esptool.py --chip esp32 -p rfc2217://c3prog.local:4000 write_flash 0x0 wt32/firmware/wt32-bridge.bin
   ```
3. Connect to `WT32-Setup-XXXX` and choose the mode and network.

## Building

After cloning: `git submodule update --init` (Berry). ESP-IDF ≥ 6.0 (tested with 6.1). In each project, `./build_firmware.sh` does a clean build and produces a full image to flash over serial at `0x0` and a `…-ota.bin` for updates through the web page.

## Credits

Written from scratch; inspired by and built on ideas from:

- [martin-ger/esp32_eth_wifi_bridge](https://github.com/martin-ger/esp32_eth_wifi_bridge) by Martin Gergeleit — the original ESP32 Ethernet ↔ Wi-Fi AP bridge this project started from; the Router and Access point modes use the same idea (an lwIP bridge of Ethernet and the AP), re-implemented here;
- [martin-ger/esp32_nat_router](https://github.com/martin-ger/esp32_nat_router), which that project grew out of;
- the ESP-IDF example [`examples/network/sta2eth`](https://github.com/espressif/esp-idf/tree/master/examples/network/sta2eth) — rewriting the device's MAC to the station's for the Client mode;
- [egnor/wt32-eth01](https://github.com/egnor/wt32-eth01) — notes on the WT32-ETH01 and its 3D model, which the case checks use (downloaded at build time, not included).

Third-party parts keep their own licenses: [Berry](https://github.com/berry-lang/berry) (MIT, git submodule), the misc-fixed 6×10 font (public domain, via [u8g2](https://github.com/olikraus/u8g2)); ESP-IDF, esp-protocols (mdns), esp-eth-drivers and rfc2217-server (Apache-2.0) are fetched at build time.

## License

[MIT](LICENSE).
