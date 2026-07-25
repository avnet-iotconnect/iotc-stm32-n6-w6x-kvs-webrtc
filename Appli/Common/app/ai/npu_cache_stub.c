/*
 * npu_cache_stub.c — register-level NPU cache (CACHEAXI) driver.
 *
 * The real npu_cache.c (Appli/Libraries/AI_Runtime/Npu/Devices/STM32N6xx)
 * needs the STM32N6 CACHEAXI HAL module, which this firmware's vendored HAL
 * copy does not ship (adding it pulls a newer HAL header set that clashes
 * with the rest of the tree).  This file provides the same symbols
 * implemented directly against the CMSIS CACHEAXI registers, mirroring
 * HAL_CACHEAXI_{Enable,CleanByAddr,CleanInvalidByAddr} and the donor's
 * npu_cache.c (x-cube-n6-ai-h264-usb-uvc).
 *
 * This is NOT just a throughput optimization: with the CACHEAXI block
 * unclocked, the NPU's external-memory fetches (weights in XSPI NOR at
 * 0x70380000, activations, ec blobs) stall and stai_network_run() never
 * completes — observed as the AiDetect task pinned at 40-65% CPU after
 * "[AI] ready" with no inference ever finishing.
 */

#if defined( ENABLE_AI_DETECTION )

#include <stdint.h>
#include "stm32n6xx_hal.h"
#include "npu_cache.h"

/* Bounded busy-wait: CACHEAXI ops complete in µs; 10M iterations is a
 * generous ceiling that still returns (with the cache left disabled)
 * rather than wedging the caller if the block misbehaves. */
#define NPU_CACHE_SPIN_LIMIT    ( 10000000UL )

static uint8_t ucCacheEnabled = 0U;

/* Bring-up telemetry, printed raw by ai_detection.c:
 * 1=clocked, 2=post-reset invalidation done, 3=enabled, 0xEE=busy timeout */
volatile uint32_t g_npu_cache_state = 0U;

static int prvWaitFlagsClear( uint32_t ulMask )
{
    for( uint32_t i = 0; i < NPU_CACHE_SPIN_LIMIT; i++ )
    {
        if( ( CACHEAXI->SR & ulMask ) == 0U )
        {
            return 0;
        }
    }

    return -1;
}

void npu_cache_enable_clocks_and_reset( void )
{
    __HAL_RCC_CACHEAXIRAM_MEM_CLK_ENABLE();
    __HAL_RCC_CACHEAXI_CLK_ENABLE();
    __HAL_RCC_CACHEAXI_FORCE_RESET();
    __HAL_RCC_CACHEAXI_RELEASE_RESET();
}

void npu_cache_disable_clocks_and_reset( void )
{
    __HAL_RCC_CACHEAXI_FORCE_RESET();
    __HAL_RCC_CACHEAXI_RELEASE_RESET();
    __HAL_RCC_CACHEAXI_CLK_DISABLE();
    __HAL_RCC_CACHEAXIRAM_MEM_CLK_DISABLE();
}

void npu_cache_enable( void )
{
    if( ucCacheEnabled != 0U )
    {
        return;
    }

    /* Power up the NPU-adjacent SRAMs (donor NPURam_enable): RAMCFG
     * shutdown bits cleared for AXISRAM3-6.  (No RAMCFG instance exists
     * for CACHEAXIRAM — its RCC MEM clock below is the whole story.) */
    __HAL_RCC_RAMCFG_CLK_ENABLE();
    CLEAR_BIT( RAMCFG_SRAM3_AXI->CR, RAMCFG_CR_SRAMSD );
    CLEAR_BIT( RAMCFG_SRAM4_AXI->CR, RAMCFG_CR_SRAMSD );
    CLEAR_BIT( RAMCFG_SRAM5_AXI->CR, RAMCFG_CR_SRAMSD );
    CLEAR_BIT( RAMCFG_SRAM6_AXI->CR, RAMCFG_CR_SRAMSD );

    npu_cache_enable_clocks_and_reset();

    /* Settle after reset release before the first register access — a
     * cold boot bus-faulted on an immediate CACHEAXI->SR read (BFAR at
     * the SR address) while a warm boot did not. */
    __DSB();

    for( volatile uint32_t i = 0; i < 1000U; i++ )
    {
    }

    g_npu_cache_state = 1U;

    /* Post-reset the block runs an automatic full invalidation (BUSYF);
     * EN must not be set until it completes. */
    if( prvWaitFlagsClear( CACHEAXI_SR_BUSYF ) != 0 )
    {
        g_npu_cache_state = 0xEEU;
        return;
    }

    g_npu_cache_state = 2U;
    SET_BIT( CACHEAXI->CR1, CACHEAXI_CR1_EN );
    g_npu_cache_state = 3U;
    ucCacheEnabled = 1U;
}

void npu_cache_disable( void )
{
    if( ucCacheEnabled == 0U )
    {
        return;
    }

    CLEAR_BIT( CACHEAXI->CR1, CACHEAXI_CR1_EN );
    npu_cache_disable_clocks_and_reset();
    ucCacheEnabled = 0U;
}

void npu_cache_invalidate( void )
{
    if( ucCacheEnabled == 0U )
    {
        return;
    }

    /* Full invalidate: launch and wait for the busy phase to end. */
    SET_BIT( CACHEAXI->CR1, CACHEAXI_CR1_CACHEINV );
    ( void ) prvWaitFlagsClear( CACHEAXI_SR_BUSYF );
}

/* Range command engine — mirrors the HAL's CACHEAXI_CommandByAddr in
 * polling mode (stm32n6xx_hal_cacheaxi.c). */
static void prvRangeCommand( uint32_t ulCmd, uint32_t ulStart, uint32_t ulEnd )
{
    if( ( ucCacheEnabled == 0U ) || ( ulStart >= ulEnd ) )
    {
        return;
    }

    if( prvWaitFlagsClear( CACHEAXI_SR_BUSYF | CACHEAXI_SR_BUSYCMDF ) != 0 )
    {
        return;
    }

    WRITE_REG( CACHEAXI->FCR, CACHEAXI_FCR_CBSYENDF | CACHEAXI_FCR_CCMDENDF );
    WRITE_REG( CACHEAXI->CMDRSADDRR, ulStart );
    WRITE_REG( CACHEAXI->CMDREADDRR, ulEnd - 1U );
    MODIFY_REG( CACHEAXI->CR2, CACHEAXI_CR2_CACHECMD, ulCmd );
    CLEAR_BIT( CACHEAXI->IER, CACHEAXI_IER_CMDENDIE );
    SET_BIT( CACHEAXI->CR2, CACHEAXI_CR2_STARTCMD );

    for( uint32_t i = 0; i < NPU_CACHE_SPIN_LIMIT; i++ )
    {
        if( ( CACHEAXI->SR & CACHEAXI_SR_CMDENDF ) != 0U )
        {
            break;
        }
    }

    WRITE_REG( CACHEAXI->FCR, CACHEAXI_FCR_CCMDENDF );
}

void npu_cache_clean_range( uint32_t start, uint32_t end )
{
    prvRangeCommand( CACHEAXI_CR2_CACHECMD_0, start, end );
}

void npu_cache_clean_invalidate_range( uint32_t start, uint32_t end )
{
    prvRangeCommand( CACHEAXI_CR2_CACHECMD_0 | CACHEAXI_CR2_CACHECMD_1, start, end );
}

#endif /* ENABLE_AI_DETECTION */
