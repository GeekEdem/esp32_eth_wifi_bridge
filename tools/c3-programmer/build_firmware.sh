#!/bin/bash
# Clean build of the C3 programmer and a single merged image in firmware/,
# flashable at offset 0x0 (esptool or a browser flasher), plus the
# application image for updates through the web page.
set -e
cd "$(dirname "$0")"

rm -rf build sdkconfig
idf.py set-target esp32c3
idf.py build

mkdir -p firmware
(cd build && python -m esptool --chip esp32c3 merge_bin -o ../firmware/c3-programmer.bin @flash_args)
# Application image alone: for the update through the web page
cp build/$(sed -n 's/^project(\(.*\))$/\1/p' CMakeLists.txt).bin firmware/c3-programmer-ota.bin
ls -la firmware/c3-programmer.bin firmware/c3-programmer-ota.bin
