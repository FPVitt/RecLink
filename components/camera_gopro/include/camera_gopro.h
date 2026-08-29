// camera_gopro.h -- Open GoPro BLE driver (spec 6). HERO 11+.
#pragma once
#include "camera.h"

// Returns the singleton driver. app_main registers it:
//   camera_register(gopro_driver());
const camera_driver_t *gopro_driver(void);
