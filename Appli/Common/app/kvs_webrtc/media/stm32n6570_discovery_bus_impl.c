/**
 * stm32n6570_discovery_bus_impl.c — HAL-based BSP I2C1 implementation.
 *
 * Provides the BSP_I2C1_* functions expected by Camera Middleware (cmw_io.h).
 * Uses I2C1 at 400 kHz (fast mode) — the standard speed for the camera
 * sensors on the STM32N6570-DK board (MB1736).
 *
 * SPDX-License-Identifier: LicenseRef-STMicroelectronics
 */

#include "stm32n6570_discovery_bus.h"
#include "stm32n6xx_hal.h"

/* ── Private state ───────────────────────────────────────────────────────── */
static I2C_HandleTypeDef hi2c1;
static uint32_t          ulI2C1InitCount = 0;

/* ── MSP helpers (GPIO / clock) ─────────────────────────────────────────── */
static void prvI2C1_MspInit( I2C_HandleTypeDef * phi2c )
{
    GPIO_InitTypeDef xGpio = { 0 };
    ( void ) phi2c;

    /* I2C1 SDA = PB9, SCL = PB8 (standard DK pins for I2C1) */
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_I2C1_CLK_ENABLE();
    __HAL_RCC_I2C1_FORCE_RESET();
    __HAL_RCC_I2C1_RELEASE_RESET();

    xGpio.Pin       = GPIO_PIN_8 | GPIO_PIN_9;
    xGpio.Mode      = GPIO_MODE_AF_OD;
    xGpio.Pull      = GPIO_NOPULL;
    xGpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    xGpio.Alternate = GPIO_AF4_I2C1;
    HAL_GPIO_Init( GPIOB, &xGpio );
}

static void prvI2C1_MspDeInit( I2C_HandleTypeDef * phi2c )
{
    ( void ) phi2c;
    __HAL_RCC_I2C1_FORCE_RESET();
    HAL_GPIO_DeInit( GPIOB, GPIO_PIN_8 | GPIO_PIN_9 );
    __HAL_RCC_I2C1_CLK_DISABLE();
}

/* ── BSP public API ──────────────────────────────────────────────────────── */

int32_t BSP_I2C1_Init( void )
{
    if( ulI2C1InitCount == 0 )
    {
        hi2c1.Instance              = I2C1;
        hi2c1.Init.Timing           = 0x00B03FDB; /* ~400 kHz @ 64 MHz I2C clk */
        hi2c1.Init.OwnAddress1      = 0;
        hi2c1.Init.AddressingMode   = I2C_ADDRESSINGMODE_7BIT;
        hi2c1.Init.DualAddressMode  = I2C_DUALADDRESS_DISABLE;
        hi2c1.Init.OwnAddress2      = 0;
        hi2c1.Init.GeneralCallMode  = I2C_GENERALCALL_DISABLE;
        hi2c1.Init.NoStretchMode    = I2C_NOSTRETCH_DISABLE;

        prvI2C1_MspInit( &hi2c1 );

        if( HAL_I2C_Init( &hi2c1 ) != HAL_OK )
        {
            return -1;
        }
    }
    ulI2C1InitCount++;
    return 0;
}

int32_t BSP_I2C1_DeInit( void )
{
    if( ulI2C1InitCount > 0 )
    {
        ulI2C1InitCount--;
        if( ulI2C1InitCount == 0 )
        {
            if( HAL_I2C_DeInit( &hi2c1 ) != HAL_OK )
            {
                return -1;
            }
            prvI2C1_MspDeInit( &hi2c1 );
        }
    }
    return 0;
}

int32_t BSP_I2C1_WriteReg( uint16_t DevAddr, uint16_t Reg, uint8_t * pData, uint16_t Length )
{
    uint8_t ucBuf[ 1 + 256 ];
    uint16_t usLen;

    if( Length > sizeof( ucBuf ) - 1 )
    {
        return -1;
    }
    ucBuf[ 0 ] = ( uint8_t ) Reg;
    for( usLen = 0; usLen < Length; usLen++ )
    {
        ucBuf[ 1 + usLen ] = pData[ usLen ];
    }
    if( HAL_I2C_Master_Transmit( &hi2c1, DevAddr, ucBuf, 1 + Length, 1000 ) != HAL_OK )
    {
        return -1;
    }
    return 0;
}

int32_t BSP_I2C1_ReadReg( uint16_t DevAddr, uint16_t Reg, uint8_t * pData, uint16_t Length )
{
    uint8_t ucReg = ( uint8_t ) Reg;

    if( HAL_I2C_Master_Transmit( &hi2c1, DevAddr, &ucReg, 1, 1000 ) != HAL_OK )
    {
        return -1;
    }
    if( HAL_I2C_Master_Receive( &hi2c1, DevAddr, pData, Length, 1000 ) != HAL_OK )
    {
        return -1;
    }
    return 0;
}

int32_t BSP_I2C1_WriteReg16( uint16_t DevAddr, uint16_t Reg, uint8_t * pData, uint16_t Length )
{
    uint8_t ucBuf[ 2 + 256 ];
    uint16_t usLen;

    if( Length > sizeof( ucBuf ) - 2 )
    {
        return -1;
    }
    ucBuf[ 0 ] = ( uint8_t )( Reg >> 8 );
    ucBuf[ 1 ] = ( uint8_t )( Reg & 0xFF );
    for( usLen = 0; usLen < Length; usLen++ )
    {
        ucBuf[ 2 + usLen ] = pData[ usLen ];
    }
    if( HAL_I2C_Master_Transmit( &hi2c1, DevAddr, ucBuf, 2 + Length, 1000 ) != HAL_OK )
    {
        return -1;
    }
    return 0;
}

int32_t BSP_I2C1_ReadReg16( uint16_t DevAddr, uint16_t Reg, uint8_t * pData, uint16_t Length )
{
    uint8_t ucReg[ 2 ];

    ucReg[ 0 ] = ( uint8_t )( Reg >> 8 );
    ucReg[ 1 ] = ( uint8_t )( Reg & 0xFF );
    if( HAL_I2C_Master_Transmit( &hi2c1, DevAddr, ucReg, 2, 1000 ) != HAL_OK )
    {
        return -1;
    }
    if( HAL_I2C_Master_Receive( &hi2c1, DevAddr, pData, Length, 1000 ) != HAL_OK )
    {
        return -1;
    }
    return 0;
}
