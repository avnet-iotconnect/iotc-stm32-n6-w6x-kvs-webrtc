/**
 * cmw_camera_conf.h — Camera Middleware configuration for STM32N6570-DK
 *                      KVS WebRTC project.
 *
 * Adapted from the template.  The BSP bus layer is not present in this
 * project; the I2C handles are accessed directly through the HAL.
 *
 * Copyright (c) 2024 STMicroelectronics.  All rights reserved.
 * SPDX-License-Identifier: LicenseRef-STMicroelectronics
 */

#ifndef CMW_CAMERA_CONF_H
#define CMW_CAMERA_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32n6xx_hal.h"
#include "stm32n6570_discovery_bus.h"

/* ── Sensor selection ────────────────────────────────────────────────────── */
/* Enable the sensors actually fitted on the STM32N6570-DK board.            */
#define USE_VD66GY_SENSOR
#define USE_IMX335_SENSOR
#define USE_VD55G1_SENSOR

#ifdef __cplusplus
}
#endif

#endif /* CMW_CAMERA_CONF_H */
