# WT32-ETH01: Wi-Fi for one Ethernet device (client or own-network mode)
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "0.7.2")
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../shared")
# Build only main and what it needs from ../shared
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wt32_bridge)
