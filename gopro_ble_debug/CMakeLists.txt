# GOPRO-BLE-DEBUG -- standalone Open GoPro BLE debug firmware for RecLink hardware.
# Build:  idf.py set-target esp32c3   (RecLink board)  |  esp32s3 (dev)
#         idf.py build
# Upload build/gopro_ble_debug.bin through the ORIGINAL RecLink portal
# (Firmware update) -- see README.md. Uses the same partition table as the
# original firmware (../partitions.csv), so the two images swap via OTA freely.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(PROJECT_VER "dbg-0.1.0")

project(gopro_ble_debug)
