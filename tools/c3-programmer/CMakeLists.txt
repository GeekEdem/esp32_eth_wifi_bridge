# ESP32-C3 SuperMini as a programmer/debug probe for WT32-ETH01
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "0.4.5")
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../shared")
# Build only main and what it needs from ../../shared
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(c3_programmer)
