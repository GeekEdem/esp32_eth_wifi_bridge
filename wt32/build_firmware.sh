#!/bin/bash
# Clean build of the WT32 bridge and a single merged image in firmware/,
# flashable at offset 0x0 (esptool or a browser flasher), plus the
# application image for updates through the web page.
set -e
cd "$(dirname "$0")"

# Berry lives in a git submodule
git -C .. submodule update --init shared/script_berry/berry 2>/dev/null || true

rm -rf build sdkconfig
idf.py set-target esp32
idf.py build

mkdir -p firmware
(cd build && python -m esptool --chip esp32 merge_bin -o ../firmware/wt32-bridge.bin @flash_args)
# Application image alone: for the update through the web page
cp build/$(sed -n 's/^project(\(.*\))$/\1/p' CMakeLists.txt).bin firmware/wt32-bridge-ota.bin
ls -la firmware/wt32-bridge.bin firmware/wt32-bridge-ota.bin
