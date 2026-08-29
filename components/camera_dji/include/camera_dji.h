// camera_dji.h -- DJI DUML BLE driver (spec 7). Osmo Action 4/5 Pro/6, Osmo 360.
#pragma once
#include "camera.h"

// app_main registers it:  camera_register(dji_driver());
const camera_driver_t *dji_driver(void);
