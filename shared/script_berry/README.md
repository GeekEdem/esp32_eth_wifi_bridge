# Berry scripts

*[Українська](README_UA.md)*

This component runs **one** user script in [Berry](https://github.com/berry-lang/berry), a lightweight language for microcontrollers (used by Tasmota). The script is written and started on the device's web page, in the **Script (Berry)** section (its texts: [`i18n/`](i18n/)).

> Status: built into the WT32 firmware; the language side (prelude, timers, limits, errors) has been tested on a PC with real Berry and this configuration, and the page in a browser. **Not verified** on hardware.

## Script API

| Call | What it does |
|---|---|
| `print(...)` | a line in the page console (and in the UART log) |
| `output(key, value)` | a result in the **Results** table (up to 16 keys; they are also shown on the display) |
| `output(key)` | removes a result |
| `every(ms, fn)` → id | call `fn` every `ms` |
| `after(ms, fn)` → id | call `fn` once after `ms` |
| `cancel(id)` | cancel a timer |
| `status()` → map | device state: the same fields as on the page (`mode`, `eth`, `devIp`, `devMac`, …) |
| `millis()` | ms since start |
| `heap()` | free device memory, bytes (the same figure as "free on the device" on the page) |

Standard modules: `string`, `json`, `math`, `time`, `global`, `gc`, `introspect`, `strict` (via `import`). There is no file system.

```berry
def show()
  var s = status()
  output("IP пристрою", s["devIp"] != "" ? s["devIp"] : "—")
end
show()
every(5000, show)
```

## Limits and protection

- **Memory**: the script budget is 40 KB (`menuconfig` → `WT32_SCRIPT_MEM_KB`). Going over it raises `memory_error`, the script stops and its memory is freed completely; Wi-Fi, Ethernet and the page are not affected.
- **Time**: one run of script code (the top level or one timer) may take up to 2 s. An endless loop is stopped with `timeout_error`.
- **Errors**: any unhandled error stops the script; the page shows the type, message and line (`script:12: …`).
- **Crashes**: if the device restarted after a fault while script code was running, autostart is turned off (the page says so).
- **Size**: up to 32 KB of text; stored in the `storage` partition (with a checksum) and compiled directly from flash.
- The script runs in its own task on the second core at low priority.

## How it works

| File | What it does |
|---|---|
| `berry/` | Berry (git submodule, pinned to a commit) |
| `port/berry_conf.h` | configuration: 32-bit integers, float, no FS/OS modules, instruction counter for the time limit, budget allocator |
| `port/be_port.c`, `be_modtab.c` | console output, file stubs, module list |
| `prelude.be` | timers and `status()`, written in Berry itself |
| `script_runtime.c` | task, limits, console, results, crash protection |
| `script_store.c` | script text in the `storage` partition |
| `script_web.c`, `script.js` | `/api/script*` routes and the page section |

Berry's constant tables are generated at build time by its own `tools/coc` tool (Python from the ESP-IDF environment).

**After cloning the repository:** `git submodule update --init` (or `git clone --recursive`). `wt32/build_firmware.sh` does this itself.

## Host test

```bash
shared/script_berry/test/run_host_test.sh
```
Builds Berry with `port/berry_conf.h` and checks: the example from the WT32 page, the `every/after/cancel` timers, stopping an endless loop, the memory budget (with complete release), a syntax error with its line number.
