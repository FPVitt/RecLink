# FC <-> Action-Cam Bridge  --  top-level project file
# Build:  idf.py set-target esp32c3   (production)  |  esp32s3 (dev)
#         idf.py build
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Firmware version string surfaced in /api/status.fw and the OTA image header.
set(PROJECT_VER "0.2.0")

project(fc_cam_bridge)
