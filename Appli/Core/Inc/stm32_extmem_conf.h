/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : stm32_extmem_conf.h
  * @version        : 1.0.0
  * @brief          : Header for extmem.c file.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
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
#ifndef __STM32_EXTMEM_CONF__H__
#define __STM32_EXTMEM_CONF__H__

#ifdef __cplusplus
 extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/

/*
  @brief management of the driver layer enable
*/

#define EXTMEM_DRIVER_NOR_SFDP   1
#define EXTMEM_DRIVER_PSRAM      0
#define EXTMEM_DRIVER_SDCARD     0
#define EXTMEM_DRIVER_USER       0

/*
  @brief management of the sal layer enable
*/
#define EXTMEM_SAL_XSPI   1
#define EXTMEM_SAL_SD     0

/* Includes ------------------------------------------------------------------*/
#include "stm32n6xx_hal.h"
#include "stm32_extmem.h"
#include "stm32_extmem_type.h"

/* USER CODE BEGIN INCLUDE */
/* Raw-UART (LPUART1, blocking) trace hook for the ExtMem middleware —
 * implemented in extmem_manager.c and gated there by g_extmem_trace so
 * the SFDP init sequence is visible on the console but steady-state
 * littlefs traffic stays quiet.  Level 2 = NOR SFDP init step/error
 * strings only (SAL level stays 0: its macros need integer formatters). */
void vExtMemRawDebug(const char *pcStr);
#define EXTMEM_MACRO_DEBUG(_str_)           vExtMemRawDebug(_str_)
#define EXTMEM_DRIVER_NOR_SFDP_DEBUG_LEVEL  2
/* USER CODE END INCLUDE */
/* Private variables ---------------------------------------------------------*/
extern XSPI_HandleTypeDef hxspi2;

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Exported constants --------------------------------------------------------*/
/** @defgroup EXTMEM_CONF_Exported_constants EXTMEM_CONF exported constants
  * @{
  */
enum {
  EXTMEMORY_1  = 0 /*!< ID=0 for the first memory  */
};

/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported configuration --------------------------------------------------------*/
/** @defgroup EXTMEM_CONF_Exported_configuration EXTMEM_CONF exported configuration definition
  * @{
  */

extern EXTMEM_DefinitionTypeDef extmem_list_config[1];
#if defined(EXTMEM_C)
EXTMEM_DefinitionTypeDef extmem_list_config[1];
#endif /* EXTMEM_C */

/**
  * @}
  */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/*
 * -- Insert your variables declaration here --
 */
/* USER CODE BEGIN VARIABLES */

/* USER CODE END VARIABLES */

/*
 * -- Insert functions declaration here --
 */
/* USER CODE BEGIN FD */

/* USER CODE END FD */

#ifdef __cplusplus
}
#endif

#endif /* __STM32_EXTMEM_CONF__H__ */
