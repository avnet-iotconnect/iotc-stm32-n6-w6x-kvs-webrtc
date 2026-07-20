/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    w61_driver_config.h
  * @author  GPM Application Team
  * @brief   Header file for the W61 configuration module
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef W61_DRIVER_CONFIG_H
#define W61_DRIVER_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* Includes ------------------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* SPI transport TX queue depth (spi_iface.c default 8, #ifndef-guarded and
 * this header IS on its include chain via w61_default_config.h).  ALL
 * traffic types (AT, network STA/AP) share this single queue; 8 slots
 * absorb <160 ms of RTP at 50-100 pkt/s, so any transient module
 * flow-control stall (rx_stall) instantly backed the whole stack up.
 * 32 slots ride out ~0.6 s bursts; worst-case transient heap cost is
 * 32 x ~1.5 KB payload copies. (2026-07-20 W6X TX-stall deep dive) */
#define SPI_TXQ_LEN    32U

/* USER CODE END Includes */

/* Exported constants --------------------------------------------------------*/
/** ============================
  * AT Common
  * All available configuration defines in
  * Middlewares\ST\ST67W6X_Network_Driver\Driver\W61_at\w61_at_common.h
  * ============================
  */
/** Maximum SPI buffer size */
#define W61_MAX_SPI_XFER                        1520

/** Enable/Disable System module logging */
#define SYS_LOG_ENABLE                          1

/** Enable/Disable Wi-Fi module logging (v1.3.0 per-subsystem flag, default 0) */
#define WIFI_LOG_ENABLE                         1

/** Enable/Disable Network module logging (v1.3.0 per-subsystem flag, default 0) */
#define NET_LOG_ENABLE                          1

/** Debugging only: Enable/Disable AT log, i.e. logs the AT commands incoming/outcoming from/to the NCP */
#define W61_AT_LOG_ENABLE                       0
#include "logging.h"

/** Enable/Disable Modem command log */
#define MDM_CMD_LOG_ENABLE                      0

/* USER CODE BEGIN EC */

/* USER CODE END EC */

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* W61_DRIVER_CONFIG_H */
