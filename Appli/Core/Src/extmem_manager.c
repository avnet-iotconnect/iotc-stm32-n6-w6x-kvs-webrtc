/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : extmem_manager.c
  * @version        : 1.0.0
  * @brief          : This file implements the extmem configuration
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

/* Includes ------------------------------------------------------------------*/
#include "extmem_manager.h"
#include <string.h>

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* USER CODE BEGIN PV */
/* Private variables ---------------------------------------------------------*/

/* USER CODE END PV */

/* USER CODE BEGIN PFP */
/* Private function prototypes -----------------------------------------------*/

/* USER CODE END PFP */

/*
 * -- Insert your variables declaration here --
 */
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/*
 * -- Insert your external function declaration here --
 */
/* USER CODE BEGIN 1 */

/* Raw LPUART1 output — runs pre-scheduler where the queued logger is not
 * available yet; bounded-spin pattern mirrors mp_raw_putc/prvAiRawPutc
 * (LPUART1 ISR @0x56000C1C bit7, TDR @0x56000C28). */
static void prvXmemRawPutc( char c )
{
  for( uint32_t i = 0; i < 600000UL; i++ )
  {
    if( *( volatile uint32_t * ) 0x56000C1CUL & ( 1UL << 7 ) )
    {
      *( volatile uint32_t * ) 0x56000C28UL = ( uint32_t ) c;
      return;
    }
  }
}

static void prvXmemRaw( const char * pcStr )
{
  while( *pcStr != '\0' )
  {
    if( *pcStr == '\n' )
    {
      prvXmemRawPutc( '\r' );
    }
    prvXmemRawPutc( *pcStr++ );
  }
}

static void prvXmemRawHex8( uint8_t ucVal )
{
  static const char hex[] = "0123456789ABCDEF";
  prvXmemRawPutc( hex[ ( ucVal >> 4 ) & 0xFU ] );
  prvXmemRawPutc( hex[ ucVal & 0xFU ] );
}

/* Gates the ExtMem MW SFDP trace (EXTMEM_MACRO_DEBUG in
 * stm32_extmem_conf.h): on during init so link bring-up is visible,
 * off afterwards so littlefs runtime traffic stays quiet. */
static volatile uint8_t ucExtmemTrace = 1U;

void vExtMemRawDebug( const char * pcStr )
{
  if( ucExtmemTrace != 0U )
  {
    prvXmemRaw( pcStr );
  }
}

/* Sanity-check the NOR link with INDIRECT reads only — indirect commands
 * go through HAL polling loops with timeouts and cannot stall the AXI
 * bus, unlike a memory-mapped read through a mis-negotiated DTR link
 * (that freeze is exactly the IWDG boot loop this guards against).
 * Offset 0 holds the signed FSBL header: known non-blank content. */
static uint8_t prvNorLinkOk( void )
{
  uint8_t ucBuf1[ 16 ], ucBuf2[ 16 ];
  uint8_t ucAll00 = 1U, ucAllFF = 1U;

  if( EXTMEM_Read( EXTMEMORY_1, 0U, ucBuf1, sizeof( ucBuf1 ) ) != EXTMEM_OK )
  {
    prvXmemRaw( "[XMEM] probe read1 failed\n" );
    return 0U;
  }

  if( EXTMEM_Read( EXTMEMORY_1, 0U, ucBuf2, sizeof( ucBuf2 ) ) != EXTMEM_OK )
  {
    prvXmemRaw( "[XMEM] probe read2 failed\n" );
    return 0U;
  }

  prvXmemRaw( "[XMEM] probe: " );

  for( uint32_t i = 0; i < sizeof( ucBuf1 ); i++ )
  {
    prvXmemRawHex8( ucBuf1[ i ] );

    if( ucBuf1[ i ] != 0x00U ) { ucAll00 = 0U; }
    if( ucBuf1[ i ] != 0xFFU ) { ucAllFF = 0U; }
  }

  prvXmemRaw( "\n" );

  if( memcmp( ucBuf1, ucBuf2, sizeof( ucBuf1 ) ) != 0 )
  {
    prvXmemRaw( "[XMEM] probe unstable (re-read mismatch)\n" );
    return 0U;
  }

  if( ( ucAll00 != 0U ) || ( ucAllFF != 0U ) )
  {
    prvXmemRaw( "[XMEM] probe blank pattern - link dead\n" );
    return 0U;
  }

  return 1U;
}

/* USER CODE END 1 */

/**
  * Init External memory manager
  * @retval None
  */
void MX_EXTMEM_MANAGER_Init(void)
{

  /* USER CODE BEGIN MX_EXTMEM_Init_PreTreatment */

  /* USER CODE END MX_EXTMEM_Init_PreTreatment */

  /* Initialization of the memory parameters */
  memset(extmem_list_config, 0x0, sizeof(extmem_list_config));

  /* EXTMEMORY_1 */
  extmem_list_config[0].MemType = EXTMEM_NOR_SFDP;
  extmem_list_config[0].Handle = (void*)&hxspi2;
  /* 8LINES puts the MX66UW1G45G in octal DTR (8D-8D-8D) at the SFDP-declared
   * 200 MHz with DQS — ~400 MB/s through the memory-mapped window.  1LINE
   * left the NPU streaming the 30 MB model weights over single-wire SPI at
   * the SFDP driver's 50 MHz floor (~6 MB/s): every first inference stalled
   * the NoC for seconds and killed live WebRTC viewer sessions.  All 8 IO
   * pads + DQS are wired and configured in HAL_XSPI_MspInit; littlefs goes
   * through EXTMEM_* / the mapped window and is link-agnostic.
   *
   * The octal link is VERIFIED with indirect reads before anyone touches
   * the memory-mapped window: a mis-negotiated DTR link (e.g. HSLV fuse /
   * VDDIO range not set — see FSBL) freezes the AXI bus on the first
   * mapped read with no fault, no ticks, nothing until the IWDG fires.
   * On any failure fall back to the old 1-line config so the board still
   * boots, streams, and can be debugged over UART. */
  {
    EXTMEM_StatusTypeDef xRet;

    extmem_list_config[0].ConfigType = EXTMEM_LINK_CONFIG_8LINES;

    prvXmemRaw( "[XMEM] NOR init: octal DTR (8LINES)\n" );
    xRet = EXTMEM_Init(EXTMEMORY_1, HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_XSPI2));
    prvXmemRaw( "[XMEM] octal init ret=" );
    prvXmemRawHex8( ( uint8_t ) xRet );
    prvXmemRaw( "\n" );

    if( ( xRet != EXTMEM_OK ) || ( prvNorLinkOk() == 0U ) )
    {
      prvXmemRaw( "[XMEM] octal link BAD - falling back to 1-line\n" );

      extmem_list_config[0].ConfigType = EXTMEM_LINK_CONFIG_1LINE;
      xRet = EXTMEM_Init(EXTMEMORY_1, HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_XSPI2));
      prvXmemRaw( "[XMEM] 1-line init ret=" );
      prvXmemRawHex8( ( uint8_t ) xRet );
      prvXmemRaw( "\n" );

      if( ( xRet != EXTMEM_OK ) || ( prvNorLinkOk() == 0U ) )
      {
        /* littlefs mount will fail and park its task; UART/CLI stay up. */
        prvXmemRaw( "[XMEM] 1-line link BAD too - NOR unusable\n" );
      }
    }
    else
    {
      prvXmemRaw( "[XMEM] octal link OK\n" );
    }

    ucExtmemTrace = 0U;
  }

  /* USER CODE BEGIN MX_EXTMEM_Init_PostTreatment */

  /* USER CODE END MX_EXTMEM_Init_PostTreatment */
}
