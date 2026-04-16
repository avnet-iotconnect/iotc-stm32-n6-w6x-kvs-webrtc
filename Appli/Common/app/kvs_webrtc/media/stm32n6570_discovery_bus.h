/**
 * stm32n6570_discovery_bus.h — Minimal BSP I2C bus stub for KVS WebRTC project.
 *
 * The Camera Middleware (cmw_camera.c / cmw_io.h) expects BSP_I2C1_* functions
 * from the discovery-board BSP layer.  This project does not carry the full BSP;
 * this header provides just the declarations that cmw_io.h needs so that the
 * Camera Middleware can compile.  The real implementations live in
 * stm32n6570_discovery_bus_impl.c (also in this directory).
 *
 * SPDX-License-Identifier: LicenseRef-STMicroelectronics
 */

#ifndef STM32N6570_DISCOVERY_BUS_H
#define STM32N6570_DISCOVERY_BUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int32_t BSP_I2C1_Init( void );
int32_t BSP_I2C1_DeInit( void );
int32_t BSP_I2C1_WriteReg( uint16_t DevAddr, uint16_t Reg, uint8_t * pData, uint16_t Length );
int32_t BSP_I2C1_ReadReg( uint16_t DevAddr, uint16_t Reg, uint8_t * pData, uint16_t Length );
int32_t BSP_I2C1_WriteReg16( uint16_t DevAddr, uint16_t Reg, uint8_t * pData, uint16_t Length );
int32_t BSP_I2C1_ReadReg16( uint16_t DevAddr, uint16_t Reg, uint8_t * pData, uint16_t Length );

#ifdef __cplusplus
}
#endif

#endif /* STM32N6570_DISCOVERY_BUS_H */
