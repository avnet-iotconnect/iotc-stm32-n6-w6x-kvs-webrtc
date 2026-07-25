/*
 * eth_netif.c — on-board Gigabit Ethernet (ETH1 + RTL8211F on RGMII) as the
 * app's network transport, replacing the ST67W6X Wi-Fi path for wired
 * deployments (see NET_USE_ETHERNET in app_freertos.c).
 *
 * Owns tcpip_init (net_main/MX_LWIP_Init never runs when this is active),
 * brings up an lwIP netif over the N6 HAL ETH v2 API, and satisfies the
 * one load-bearing gate every consumer waits on: EVT_MASK_NET_CONNECTED on
 * xSystemEvents, set when DHCP binds an address.
 *
 * Hardware facts (from ST's STM32CubeN6 Nx_WebServer for this exact board):
 *   PHY   : RTL8211F(-CG), RGMII, MDIO on PD12 / MDC on PD1, INTN PD3.
 *   Pins  : PD1/3/12, PF0(AF12,GTX_CLK)/2/5/7-15, PG3/4 — rest AF11.
 *   Clocks: kernel = HCLK; RGMII RX/TX refclks external (PHY pads, reset
 *           default); interface select CCIPR2 written by HAL_ETH_Init.
 *
 * DMA coherency: the N6 HAL ETH driver does NO cache maintenance.  DMA
 * descriptors AND the RX buffer pool live in a small non-cacheable MPU
 * region carved from the top of app RAM (NCRAM in the linker script);
 * TX payloads stay cacheable and are cleaned per segment before transmit.
 */

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "event_groups.h"

#include "stm32n6xx_hal.h"

#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/netifapi.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "netif/ethernet.h"

#include "sys_evt.h"
#include "rtl8211.h"

#include "logging_levels.h"
#ifndef LOG_LEVEL
    #define LOG_LEVEL    LOG_INFO
#endif
#include "logging.h"

/* ── Static config ─────────────────────────────────────────────────────── */
#define ETH_RX_BUF_CNT        ( 6U )
#define ETH_RX_BUF_SIZE       ( 1536U )
#define ETH_TX_TIMEOUT_MS     ( 100U )
#define ETH_LINK_POLL_MS      ( 500U )
#define ETH_RX_POLL_MS        ( 2U )

/* Non-cacheable carve: must match NCRAM in STM32N657X0HXQ_LRUN_kvs.ld and
 * the MPU region below. */
#define ETH_NOCACHE_BASE      ( 0x341FD800UL )
#define ETH_NOCACHE_LIMIT     ( 0x341FFFFFUL )

#define NC_SECTION            __attribute__(( section( ".eth_nocache" ), aligned( 32 ) ))

/* DMA descriptor tables — one list per DMA channel (HAL default 2 Tx + 2 Rx
 * channels; only channel 0 carries traffic). */
static ETH_DMADescTypeDef xDmaTxDesc[ ETH_DMA_TX_CH_CNT ][ ETH_TX_DESC_CNT ] NC_SECTION;
static ETH_DMADescTypeDef xDmaRxDesc[ ETH_DMA_RX_CH_CNT ][ ETH_RX_DESC_CNT ] NC_SECTION;

/* RX buffer pool (non-cacheable — no invalidate needed) + free stack. */
static uint8_t  ucRxPool[ ETH_RX_BUF_CNT ][ ETH_RX_BUF_SIZE ] NC_SECTION;
static uint8_t *pucRxFree[ ETH_RX_BUF_CNT ];
static uint32_t ulRxFreeTop = 0U;

/* Per-buffer segment records for the HAL's RxLink chaining. */
typedef struct EthRxSeg
{
    uint8_t *         pucBuf;
    uint16_t          usLen;
    struct EthRxSeg * pxNext;
} EthRxSeg_t;

static EthRxSeg_t xRxSegs[ ETH_RX_BUF_CNT ];

static ETH_HandleTypeDef xEthHandle;
static rtl8211_Object_t  xPhy;
static struct netif      xEthNetif;
static volatile uint8_t  ucIpReported = 0U;

extern EventGroupHandle_t xSystemEvents;

/* ── Non-cacheable region MPU setup ────────────────────────────────────── */

static void prvEthMemSetup( void )
{
    MPU_Attributes_InitTypeDef xAttr   = { 0 };
    MPU_Region_InitTypeDef     xRegion = { 0 };

    HAL_MPU_Disable();

    xAttr.Number     = MPU_ATTRIBUTES_NUMBER0;
    xAttr.Attributes = INNER_OUTER( MPU_NOT_CACHEABLE );
    HAL_MPU_ConfigMemoryAttributes( &xAttr );

    xRegion.Enable           = MPU_REGION_ENABLE;
    xRegion.Number           = MPU_REGION_NUMBER0;
    xRegion.BaseAddress      = ETH_NOCACHE_BASE;
    xRegion.LimitAddress     = ETH_NOCACHE_LIMIT;
    xRegion.AttributesIndex  = MPU_ATTRIBUTES_NUMBER0;
    xRegion.AccessPermission = MPU_REGION_ALL_RW;
    xRegion.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;
    xRegion.IsShareable      = MPU_ACCESS_NOT_SHAREABLE;
    HAL_MPU_ConfigRegion( &xRegion );

    HAL_MPU_Enable( MPU_PRIVILEGED_DEFAULT );

    /* Unlike ST's examples (MPU set before caches), this app boots with
     * D-cache already on: evict any lines allocated over the carve while
     * it was still cacheable, then scrub it. */
    SCB_CleanInvalidateDCache_by_Addr( ( uint32_t * ) ETH_NOCACHE_BASE,
                                       ( int32_t ) ( ETH_NOCACHE_LIMIT - ETH_NOCACHE_BASE + 1U ) );

    ( void ) memset( ( void * ) ETH_NOCACHE_BASE, 0,
                     ETH_NOCACHE_LIMIT - ETH_NOCACHE_BASE + 1U );
}

/* ── HAL MSP: RIF, clocks, pins (called from HAL_ETH_Init) ─────────────── */

void HAL_ETH_MspInit( ETH_HandleTypeDef * heth )
{
    GPIO_InitTypeDef xGpio = { 0 };
    RCC_PeriphCLKInitTypeDef xClk = { 0 };

    ( void ) heth;

    /* RIF: same CID1 SEC|PRIV pattern as DCMIPP/VENC/NPU/LTDC. */
    {
        RIMC_MasterConfig_t xMaster = { 0 };

        __HAL_RCC_RIFSC_CLK_ENABLE();
        xMaster.MasterCID = RIF_CID_1;
        xMaster.SecPriv   = RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV;
        HAL_RIF_RIMC_ConfigMasterAttributes( RIF_MASTER_INDEX_ETH1, &xMaster );
        HAL_RIF_RISC_SetSlaveSecureAttributes( RIF_RISC_PERIPH_INDEX_ETH1,
                                               RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV );
    }

    /* RM0486: select the PHY interface while ETH1 is under reset, before
     * its clocks are enabled.  (HAL_ETH_Init re-writes the same value
     * later — harmless.) */
    __HAL_RCC_ETH1_FORCE_RESET();
    __HAL_RCC_ETH1PHY_CONFIG( RCC_ETH1PHYIF_RGMII );

    /* Kernel clock = HCLK (per ST's DK reference config). */
    xClk.PeriphClockSelection = RCC_PERIPHCLK_ETH1;
    xClk.Eth1ClockSelection   = RCC_ETH1CLKSOURCE_HCLK;
    ( void ) HAL_RCCEx_PeriphCLKConfig( &xClk );

    __HAL_RCC_ETH1_CLK_ENABLE();
    __HAL_RCC_ETH1MAC_CLK_ENABLE();
    __HAL_RCC_ETH1TX_CLK_ENABLE();
    __HAL_RCC_ETH1RX_CLK_ENABLE();
    __HAL_RCC_ETH1_RELEASE_RESET();

    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();

    /* TrustZone pin attributes, app convention. */
    HAL_GPIO_ConfigPinAttributes( GPIOD, GPIO_PIN_1 | GPIO_PIN_3 | GPIO_PIN_12,
                                  GPIO_PIN_SEC | GPIO_PIN_NPRIV );
    HAL_GPIO_ConfigPinAttributes( GPIOF, GPIO_PIN_0 | GPIO_PIN_2 | GPIO_PIN_5 |
                                         GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9 |
                                         GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12 |
                                         GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15,
                                  GPIO_PIN_SEC | GPIO_PIN_NPRIV );
    HAL_GPIO_ConfigPinAttributes( GPIOG, GPIO_PIN_3 | GPIO_PIN_4,
                                  GPIO_PIN_SEC | GPIO_PIN_NPRIV );

    /* PD: MDC(1), PHY_INTN(3), MDIO(12) — AF11 */
    xGpio.Mode      = GPIO_MODE_AF_PP;
    xGpio.Pull      = GPIO_NOPULL;
    xGpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    xGpio.Alternate = GPIO_AF11_ETH1;
    xGpio.Pin       = GPIO_PIN_1 | GPIO_PIN_3 | GPIO_PIN_12;
    HAL_GPIO_Init( GPIOD, &xGpio );

    /* PF0: RGMII_GTX_CLK — the one AF12 pin, medium speed per ST config */
    xGpio.Pull      = GPIO_NOPULL;
    xGpio.Speed     = GPIO_SPEED_FREQ_MEDIUM;
    xGpio.Alternate = GPIO_AF12_ETH1;
    xGpio.Pin       = GPIO_PIN_0;
    HAL_GPIO_Init( GPIOF, &xGpio );

    /* PF: CLK125(2), CLK(5), RX_CLK(7), RXD2(8), RXD3(9), RX_CTL(10),
     * TX_CTL(11), TXD0(12), TXD1(13), RXD0(14), RXD1(15) — AF11, pullup */
    xGpio.Pull      = GPIO_PULLUP;
    xGpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    xGpio.Alternate = GPIO_AF11_ETH1;
    xGpio.Pin       = GPIO_PIN_2 | GPIO_PIN_5 | GPIO_PIN_7 | GPIO_PIN_8 |
                      GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12 |
                      GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
    HAL_GPIO_Init( GPIOF, &xGpio );

    /* PG: TXD2(3), TXD3(4) — AF11, pullup */
    xGpio.Pin = GPIO_PIN_3 | GPIO_PIN_4;
    HAL_GPIO_Init( GPIOG, &xGpio );
}

/* ── HAL RX plumbing: buffer pool + segment chaining ───────────────────── */

void HAL_ETH_RxAllocateCallback( uint8_t ** ppucBuff )
{
    if( ulRxFreeTop > 0U )
    {
        ulRxFreeTop--;
        *ppucBuff = pucRxFree[ ulRxFreeTop ];
    }
    else
    {
        *ppucBuff = NULL;
    }
}

void HAL_ETH_RxLinkCallback( void ** ppvStart, void ** ppvEnd,
                             uint8_t * pucBuff, uint16_t usLength )
{
    uint32_t ulIdx = ( ( uint32_t ) pucBuff - ( uint32_t ) &ucRxPool[ 0 ][ 0 ] ) /
                     ETH_RX_BUF_SIZE;
    EthRxSeg_t * pxSeg = &xRxSegs[ ulIdx ];

    pxSeg->pucBuf = pucBuff;
    pxSeg->usLen  = usLength;
    pxSeg->pxNext = NULL;

    if( *ppvStart == NULL )
    {
        *ppvStart = pxSeg;
    }
    else
    {
        ( ( EthRxSeg_t * ) ( *ppvEnd ) )->pxNext = pxSeg;
    }

    *ppvEnd = pxSeg;
}

void HAL_ETH_TxFreeCallback( uint32_t * pulBuff )
{
    /* Blocking transmit + no dynamic TX buffers: nothing to free. */
    ( void ) pulBuff;
}

static void prvRxSegFree( EthRxSeg_t * pxSeg )
{
    while( pxSeg != NULL )
    {
        if( ulRxFreeTop < ETH_RX_BUF_CNT )
        {
            pucRxFree[ ulRxFreeTop ] = pxSeg->pucBuf;
            ulRxFreeTop++;
        }

        pxSeg = pxSeg->pxNext;
    }
}

/* ── lwIP glue ─────────────────────────────────────────────────────────── */

static err_t prvLinkOutput( struct netif * pxNetif, struct pbuf * pxPbuf )
{
    ETH_BufferTypeDef xTxBuf[ 12 ];
    ETH_TxPacketConfigTypeDef xTxCfg;
    struct pbuf * pxQ;
    uint32_t ulSeg = 0U;

    ( void ) pxNetif;

    for( pxQ = pxPbuf; pxQ != NULL; pxQ = pxQ->next )
    {
        if( ulSeg >= 12U )
        {
            return ERR_IF;
        }

        xTxBuf[ ulSeg ].buffer = pxQ->payload;
        xTxBuf[ ulSeg ].len    = pxQ->len;
        xTxBuf[ ulSeg ].next   = NULL;

        if( ulSeg > 0U )
        {
            xTxBuf[ ulSeg - 1U ].next = &xTxBuf[ ulSeg ];
        }

        /* TX payload lives in cacheable RAM — push it out for the DMA. */
        SCB_CleanDCache_by_Addr( ( uint32_t * ) ( ( uint32_t ) pxQ->payload & ~31UL ),
                                 ( int32_t ) ( pxQ->len + 64U ) );
        ulSeg++;
    }

    ( void ) memset( &xTxCfg, 0, sizeof( xTxCfg ) );
    xTxCfg.TxDMACh      = 0U;
    xTxCfg.Attributes   = ETH_TX_PACKETS_FEATURES_CRCPAD;
    xTxCfg.CRCPadCtrl   = ETH_CRC_PAD_INSERT;
    xTxCfg.ChecksumCtrl = ETH_CHECKSUM_DISABLE;
    xTxCfg.Length       = pxPbuf->tot_len;
    xTxCfg.TxBuffer     = &xTxBuf[ 0 ];
    xTxCfg.pData        = NULL;

    if( HAL_ETH_Transmit( &xEthHandle, &xTxCfg, ETH_TX_TIMEOUT_MS ) != HAL_OK )
    {
        return ERR_IF;
    }

    ( void ) HAL_ETH_ReleaseTxPacket( &xEthHandle );

    return ERR_OK;
}

static err_t prvEthNetifInit( struct netif * pxNetif )
{
    pxNetif->name[ 0 ]   = 'e';
    pxNetif->name[ 1 ]   = 't';
    pxNetif->output      = etharp_output;
    pxNetif->linkoutput  = prvLinkOutput;
    pxNetif->mtu         = 1500;
    pxNetif->flags       = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    pxNetif->hwaddr_len  = ETH_HWADDR_LEN;
    ( void ) memcpy( pxNetif->hwaddr, xEthHandle.Init.MACAddr, ETH_HWADDR_LEN );

    return ERR_OK;
}

static void prvStatusCallback( struct netif * pxNetif )
{
    if( ( netif_is_up( pxNetif ) ) &&
        ( ip4_addr_get_u32( netif_ip4_addr( pxNetif ) ) != 0U ) &&
        ( ucIpReported == 0U ) )
    {
        ucIpReported = 1U;

        LogInfo( "[ETH] IP %s GW %s",
                 ip4addr_ntoa( netif_ip4_addr( pxNetif ) ),
                 ip4addr_ntoa( netif_ip4_gw( pxNetif ) ) );

        /* The one load-bearing line: releases KVS / MQTT / IoTConnect. */
        xEventGroupSetBits( xSystemEvents, EVT_MASK_NET_CONNECTED );
    }
}

/* ── PHY MDIO glue ─────────────────────────────────────────────────────── */

static int32_t prvPhyIoInit( void ) { return 0; }
static int32_t prvPhyIoDeInit( void ) { return 0; }

static int32_t prvPhyIoRead( uint32_t ulAddr, uint32_t ulReg, uint32_t * pulVal )
{
    return ( HAL_ETH_ReadPHYRegister( &xEthHandle, ulAddr, ulReg, pulVal ) == HAL_OK ) ? 0 : -1;
}

static int32_t prvPhyIoWrite( uint32_t ulAddr, uint32_t ulReg, uint32_t ulVal )
{
    return ( HAL_ETH_WritePHYRegister( &xEthHandle, ulAddr, ulReg, ulVal ) == HAL_OK ) ? 0 : -1;
}

static int32_t prvPhyIoTick( void )
{
    return ( int32_t ) HAL_GetTick();
}

/* ── Link management ───────────────────────────────────────────────────── */

/* Returns 1 if the MAC was (re)started for a live link. */
static uint8_t prvLinkUpdate( uint8_t ucWasUp )
{
    int32_t lState = RTL8211_GetLinkState( &xPhy );
    uint8_t ucUp = 0U;

    switch( lState )
    {
        case RTL8211_STATUS_1000MBITS_FULLDUPLEX:
        case RTL8211_STATUS_1000MBITS_HALFDUPLEX:
        case RTL8211_STATUS_100MBITS_FULLDUPLEX:
        case RTL8211_STATUS_100MBITS_HALFDUPLEX:
        case RTL8211_STATUS_10MBITS_FULLDUPLEX:
        case RTL8211_STATUS_10MBITS_HALFDUPLEX:
            ucUp = 1U;
            break;

        default:
            ucUp = 0U;
            break;
    }

    if( ( ucUp != 0U ) && ( ucWasUp == 0U ) )
    {
        ETH_MACConfigTypeDef xMacCfg;

        ( void ) HAL_ETH_GetMACConfig( &xEthHandle, &xMacCfg );

        switch( lState )
        {
            case RTL8211_STATUS_1000MBITS_FULLDUPLEX:
                xMacCfg.PortSelect = DISABLE;                 /* GMII/RGMII 1000 */
                xMacCfg.Speed      = ETH_SPEED_1000M;
                xMacCfg.DuplexMode = ETH_FULLDUPLEX_MODE;
                break;

            case RTL8211_STATUS_1000MBITS_HALFDUPLEX:
                xMacCfg.PortSelect = DISABLE;
                xMacCfg.Speed      = ETH_SPEED_1000M;
                xMacCfg.DuplexMode = ETH_HALFDUPLEX_MODE;
                break;

            case RTL8211_STATUS_100MBITS_FULLDUPLEX:
                xMacCfg.PortSelect = ENABLE;
                xMacCfg.Speed      = ETH_SPEED_100M;
                xMacCfg.DuplexMode = ETH_FULLDUPLEX_MODE;
                break;

            case RTL8211_STATUS_100MBITS_HALFDUPLEX:
                xMacCfg.PortSelect = ENABLE;
                xMacCfg.Speed      = ETH_SPEED_100M;
                xMacCfg.DuplexMode = ETH_HALFDUPLEX_MODE;
                break;

            case RTL8211_STATUS_10MBITS_FULLDUPLEX:
                xMacCfg.PortSelect = ENABLE;
                xMacCfg.Speed      = ETH_SPEED_10M;
                xMacCfg.DuplexMode = ETH_FULLDUPLEX_MODE;
                break;

            default:
                xMacCfg.PortSelect = ENABLE;
                xMacCfg.Speed      = ETH_SPEED_10M;
                xMacCfg.DuplexMode = ETH_HALFDUPLEX_MODE;
                break;
        }

        ( void ) HAL_ETH_SetMACConfig( &xEthHandle, &xMacCfg );

        if( HAL_ETH_Start( &xEthHandle ) == HAL_OK )
        {
            LogInfo( "[ETH] link up (state=%ld)", lState );
            ( void ) netifapi_netif_common( &xEthNetif, netif_set_link_up, NULL );
            ( void ) netifapi_dhcp_start( &xEthNetif );
        }
        else
        {
            LogError( "[ETH] HAL_ETH_Start failed" );
            ucUp = 0U;
        }
    }
    else if( ( ucUp == 0U ) && ( ucWasUp != 0U ) )
    {
        LogWarn( "[ETH] link down" );
        ( void ) HAL_ETH_Stop( &xEthHandle );
        ( void ) netifapi_netif_common( &xEthNetif, netif_set_link_down, NULL );
    }

    return ucUp;
}

/* ── RX drain (polling) ────────────────────────────────────────────────── */

static void prvRxDrain( void )
{
    void * pvPacket = NULL;

    while( HAL_ETH_ReadData( &xEthHandle, &pvPacket ) == HAL_OK )
    {
        EthRxSeg_t * pxSeg = ( EthRxSeg_t * ) pvPacket;
        uint32_t ulTotal = 0U;
        EthRxSeg_t * pxIter;
        struct pbuf * pxPbuf;

        for( pxIter = pxSeg; pxIter != NULL; pxIter = pxIter->pxNext )
        {
            ulTotal += pxIter->usLen;
        }

        pxPbuf = pbuf_alloc( PBUF_RAW, ( uint16_t ) ulTotal, PBUF_POOL );

        if( pxPbuf != NULL )
        {
            uint32_t ulOff = 0U;

            for( pxIter = pxSeg; pxIter != NULL; pxIter = pxIter->pxNext )
            {
                ( void ) pbuf_take_at( pxPbuf, pxIter->pucBuf, pxIter->usLen,
                                       ( uint16_t ) ulOff );
                ulOff += pxIter->usLen;
            }

            if( xEthNetif.input( pxPbuf, &xEthNetif ) != ERR_OK )
            {
                pbuf_free( pxPbuf );
            }
        }

        prvRxSegFree( pxSeg );
        pvPacket = NULL;
    }
}

/* ── Main task ─────────────────────────────────────────────────────────── */

void eth_net_main( void * pvParam )
{
    static uint8_t ucMac[ 6 ];
    ip4_addr_t xIp, xMask, xGw;
    uint8_t ucLinkUp = 0U;
    uint32_t ulIdx;
    TickType_t xLastLinkPoll = 0;

    ( void ) pvParam;

    LogInfo( "[ETH] bring-up: RTL8211F RGMII, kernel=HCLK" );

    prvEthMemSetup();

    /* RX free stack */
    for( ulIdx = 0U; ulIdx < ETH_RX_BUF_CNT; ulIdx++ )
    {
        pucRxFree[ ulIdx ] = &ucRxPool[ ulIdx ][ 0 ];
    }
    ulRxFreeTop = ETH_RX_BUF_CNT;

    /* Locally-administered MAC derived from the device UID. */
    {
        uint32_t ulUid = HAL_GetUIDw0() ^ HAL_GetUIDw1() ^ HAL_GetUIDw2();

        ucMac[ 0 ] = 0x02;
        ucMac[ 1 ] = 0x80;
        ucMac[ 2 ] = 0xE1;
        ucMac[ 3 ] = ( uint8_t ) ( ulUid >> 16 );
        ucMac[ 4 ] = ( uint8_t ) ( ulUid >> 8 );
        ucMac[ 5 ] = ( uint8_t ) ( ulUid );
    }

    /* This task owns the lwIP stack (net_main/MX_LWIP_Init never runs). */
    tcpip_init( NULL, NULL );

    xEthHandle.Instance          = ETH1;
    xEthHandle.Init.MACAddr      = ucMac;
    xEthHandle.Init.MediaInterface = HAL_ETH_RGMII_MODE;
    xEthHandle.Init.RxBuffLen    = ETH_RX_BUF_SIZE;

    for( ulIdx = 0U; ulIdx < ETH_DMA_TX_CH_CNT; ulIdx++ )
    {
        xEthHandle.Init.TxDesc[ ulIdx ] = &xDmaTxDesc[ ulIdx ][ 0 ];
    }

    for( ulIdx = 0U; ulIdx < ETH_DMA_RX_CH_CNT; ulIdx++ )
    {
        xEthHandle.Init.RxDesc[ ulIdx ] = &xDmaRxDesc[ ulIdx ][ 0 ];
    }

    if( HAL_ETH_Init( &xEthHandle ) != HAL_OK )
    {
        LogError( "[ETH] HAL_ETH_Init failed (err=0x%lx) - parked",
                  xEthHandle.ErrorCode );
        vTaskSuspend( NULL );
    }

    HAL_ETH_SetMDIOClockRange( &xEthHandle );

    /* PHY bring-up (address auto-scan inside the driver). */
    {
        rtl8211_IOCtx_t xIoCtx =
        {
            .Init     = prvPhyIoInit,
            .DeInit   = prvPhyIoDeInit,
            .WriteReg = prvPhyIoWrite,
            .ReadReg  = prvPhyIoRead,
            .GetTick  = prvPhyIoTick,
        };

        ( void ) RTL8211_RegisterBusIO( &xPhy, &xIoCtx );

        if( RTL8211_Init( &xPhy ) != RTL8211_STATUS_OK )
        {
            LogError( "[ETH] RTL8211 init failed (no PHY?) - parked" );
            vTaskSuspend( NULL );
        }

        /* Post-reset BMCR default = autoneg enabled, no power-down; the
         * driver's Init already handled reset + optional RGMII delay/EEE
         * knobs (ENABLE_RTL8211F_TXDELAY etc. if bring-up needs them). */
        LogInfo( "[ETH] PHY found at addr %lu", xPhy.DevAddr );
    }

    /* Add the netif and make it the default. */
    ip4_addr_set_zero( &xIp );
    ip4_addr_set_zero( &xMask );
    ip4_addr_set_zero( &xGw );

    ( void ) netifapi_netif_add( &xEthNetif, &xIp, &xMask, &xGw, NULL,
                                 prvEthNetifInit, tcpip_input );
    netif_set_status_callback( &xEthNetif, prvStatusCallback );
    ( void ) netifapi_netif_set_default( &xEthNetif );
    ( void ) netifapi_netif_set_up( &xEthNetif );

    LogInfo( "[ETH] netif up, waiting for link/DHCP..." );

    for( ; ; )
    {
        TickType_t xNow = xTaskGetTickCount();

        if( ( xNow - xLastLinkPoll ) >= pdMS_TO_TICKS( ETH_LINK_POLL_MS ) )
        {
            xLastLinkPoll = xNow;
            ucLinkUp = prvLinkUpdate( ucLinkUp );
        }

        if( ucLinkUp != 0U )
        {
            prvRxDrain();
        }

        vTaskDelay( pdMS_TO_TICKS( ETH_RX_POLL_MS ) );
    }
}
