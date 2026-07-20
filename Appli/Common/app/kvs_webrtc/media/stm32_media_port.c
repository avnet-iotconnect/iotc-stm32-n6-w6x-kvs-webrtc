/*
 * STM32N6570 KVS WebRTC port — stm32_media_port.c
 *
 * Implements the AppMediaSourcePort_* interface required by the KVS WebRTC
 * SDK (examples/app_media_source/app_media_source_port.h).
 *
 * Architecture:
 *   DCMIPP PIPE1 → frame buffer  →  MediaEnc_EncodeFrame()  →  H.264 NAL
 *   H.264 NAL  →  onVideoFrameReadyToSendFunc callback (→ PeerConnection)
 *
 * A single FreeRTOS task drives the capture+encode loop.  Audio is not
 * available on this board; the audio callback is never invoked.
 *
 * Copyright (c) 2025 STMicroelectronics / project contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_media_source_port.h"   /* SDK interface                       */
#include "media_cam.h"
#include "media_enc.h"
#include "media_cam_config.h"        /* VENC_*_WIDTH / HEIGHT, CAMERA_FPS   */

#include "stm32n6xx_hal.h"

#include "FreeRTOS.h"
#include "task.h"

#include "logging_levels.h"
#define LOG_LEVEL   LOG_INFO
#include "logging.h"

#include "freertos_hooks.h"          /* vPetWatchdog()                       */

#include <assert.h>
#include <string.h>
#include <stdlib.h>

/* ── Raw UART debug ────────────────────────────────────────────────────── */
/* Bounded spin — see rationale in kvs_webrtc_task.c. */
extern void vPetWatchdog( void );
static inline void mp_raw_putc( char c )
{
    for( uint32_t i = 0; i < 600000UL; i++ )
    {
        if( *(volatile uint32_t *)0x56000C1CUL & ( 1UL << 7 ) )
        {
            *(volatile uint32_t *)0x56000C28UL = ( uint32_t ) c;
            return;
        }
    }
    vPetWatchdog();
}
static void mp_raw_puts( const char *s ) { while( *s ) mp_raw_putc( *s++ ); }
static void mp_raw_dec( int v )
{
    char buf[ 12 ];
    int i = 0;
    int neg = 0;
    if( v < 0 ) { neg = 1; v = -v; }
    if( v == 0 ) buf[ i++ ] = '0';
    while( v > 0 ) { buf[ i++ ] = '0' + ( v % 10 ); v /= 10; }
    if( neg ) mp_raw_putc( '-' );
    while( i-- > 0 ) mp_raw_putc( buf[ i ] );
}
static void mp_raw_hex32( uint32_t v )
{
    int i;
    for( i = 7; i >= 0; i-- )
    {
        uint32_t n = ( v >> ( i * 4 ) ) & 0xFU;
        mp_raw_putc( ( char )( n < 10 ? ( '0' + n ) : ( 'a' + n - 10 ) ) );
    }
}

/* ── Frame-content probe ────────────────────────────────────────────────────
 *
 * Sample NV12 Y-plane bytes from 5 column positions across a single row
 * near the middle of the frame and dump them to UART.  If DCMIPP is
 * actually writing the full 1280-pixel row, all 5 should look like
 * plausible luma values (roughly 16..235 for an exposed scene).  If
 * DCMIPP is starving on PSRAM bandwidth and truncating rows, columns
 * past the truncation point will show stale/uninitialised memory —
 * typically repeated patterns or all 0xff / 0x00.                          */
static void mp_probe_frame_row( const uint8_t * pY )
{
    /* 1280 Y-bytes per row; sample row 360 (middle of 720p Y-plane).       */
    const uint32_t ulRowBytes = 1280U;
    const uint32_t ulRowY     = 360U;
    const uint32_t ulRowBase  = ulRowY * ulRowBytes;
    const uint32_t ulCols[ 5 ] = { 0U, 320U, 640U, 960U, 1270U };
    int lI;

    mp_raw_puts( "[M]Y360:" );
    for( lI = 0; lI < 5; lI++ )
    {
        uint8_t ucY = pY[ ulRowBase + ulCols[ lI ] ];
        mp_raw_puts( " c" );
        mp_raw_dec( ( int ) ulCols[ lI ] );
        mp_raw_putc( '=' );
        mp_raw_hex32( ( uint32_t ) ucY );
    }
    mp_raw_puts( "\r\n" );
}

/* ── Configuration ──────────────────────────────────────────────────────── */

/* NV12 raw frame buffers.  At 1280×720 the Y plane is 921,600 bytes and
 * the interleaved UV plane is 460,800 bytes (1.5 bytes/pixel total).  Two
 * ping-pong pairs are allocated for DCMIPP double-buffer mode — DCMIPP
 * alternates between the pair-0 and pair-1 addresses on every VSYNC and
 * the media task encodes the just-completed pair while the other one is
 * being written.                                                          */
#define FRAME_BUF_WIDTH        ( 1280U )
#define FRAME_BUF_HEIGHT       ( 720U )
#define Y_PLANE_BYTES          ( FRAME_BUF_WIDTH * FRAME_BUF_HEIGHT )        /* 921 600 */
#define UV_PLANE_BYTES         ( FRAME_BUF_WIDTH * ( FRAME_BUF_HEIGHT / 2U ) ) /* 460 800 */

/* Encoded H.264 output buffer.  Observed worst-case keyframe size is
 * ~770 KB at 720p; 1.5 MB gives ample margin without wasting PSRAM.      */
#define ENCODED_BUF_BYTES      ( 1536U * 1024U )

/* 16-byte alignment is required by HAL_DCMIPP_CSI_PIPE_SemiPlanarDouble-
 * BufferStart, which rejects unaligned Y/UV addresses.  32-byte alignment
 * keeps cache-line behaviour clean on the M55 D-cache.                    */
static uint8_t ucYPlaneBuf0[ Y_PLANE_BYTES ]
    __attribute__(( section(".psram_bss"), aligned(32) ));
static uint8_t ucYPlaneBuf1[ Y_PLANE_BYTES ]
    __attribute__(( section(".psram_bss"), aligned(32) ));
static uint8_t ucUVPlaneBuf0[ UV_PLANE_BYTES ]
    __attribute__(( section(".psram_bss"), aligned(32) ));
static uint8_t ucUVPlaneBuf1[ UV_PLANE_BYTES ]
    __attribute__(( section(".psram_bss"), aligned(32) ));
static uint8_t ucEncodedFrameBuf[ ENCODED_BUF_BYTES ]
    __attribute__(( section(".psram_bss"), aligned(32) ));

#define MEDIA_TASK_STACK_DEPTH  ( 4096U )
#define MEDIA_TASK_PRIORITY     ( tskIDLE_PRIORITY + 3U )

/* ── Private state ──────────────────────────────────────────────────────── */

typedef struct
{
    OnFrameReadyToSend_t  pfnOnVideoFrame;
    void *                pvVideoCtx;
    TaskHandle_t          xTaskHandle;
    volatile uint8_t      ucRunning;

    uint8_t * pucEncodedFrame;
} MediaPortCtx_t;

static MediaPortCtx_t xMediaCtx;

/* ── Capture+encode loop task ───────────────────────────────────────────── */
/*
 * Strategy: DCMIPP double-buffer (DBM) mode.  The camera pipeline is started
 * with two PSRAM frame buffers (ucRawFrameBuf0 / ucRawFrameBuf1) and DCMIPP
 * alternates between them on every VSYNC.  MediaCam_WaitFrame() blocks on a
 * binary semaphore signalled from the DCMIPP frame-event ISR and returns a
 * pointer to the buffer that was JUST completed — the one DCMIPP is not
 * currently writing.  VENC encodes from that stable buffer while the next
 * frame streams into the other one.  Result: zero tearing, full 15 fps
 * (provided encode + transmit < frame period).
 */

static void prvMediaTask( void * pvParam )
{
    MediaPortCtx_t * pxCtx       = ( MediaPortCtx_t * ) pvParam;
    MediaFrame_t     xFrame;
    int              lEncodedLen;
    uint64_t         ullTimestampUs = 0ULL;
    const uint32_t   ulFramePeriodUs = 1000000UL / CAMERA_FPS;

    LogInfo( "[KVSMedia] Media task started — DCMIPP double-buffer mode." );

    /* Start DCMIPP in ping-pong double-buffer mode with NV12 semi-planar
     * output.  Hardware alternates writes between the (Y0,UV0) pair and
     * the (Y1,UV1) pair on each VSYNC.                                    */
    mp_raw_puts( "[KVSMedia] cam DBL start>\r\n" );
    MediaCam_StartDouble( ucYPlaneBuf0, ucUVPlaneBuf0,
                          ucYPlaneBuf1, ucUVPlaneBuf1,
                          CMW_MODE_CONTINUOUS );
    mp_raw_puts( "[KVSMedia] cam DBL started\r\n" );

    /* PIPE2 (NPU input) rides the same sensor: it can only start once the
     * camera is actually running — starting it from AI init at boot failed
     * (sensor idle until a viewer connects) and left the AI task wedged.
     * No-op unless ENABLE_AI_DETECTION.  Stop is in media_cam.c Stop. */
    {
        extern void AiDetection_PipeStart( void );

        /* BISECT (2026-07-20) step 1 RESULT: PIPE2 off -> sessions survive
         * (160s+, isnd 26ms vs ~1100ms); platform work exonerated, AI path
         * confirmed as the killer.  Step 2: PIPE2 back ON with inference
         * skipped in ai_detection.c (AI_BISECT_SKIP_INFERENCE) to split
         * DCMIPP-PIPE2 traffic from NPU inference bursts. */
        AiDetection_PipeStart();
    }

    /* Hot-path UART traces removed.  At 115200 baud each byte is ~87us,
     * so the ~120 bytes of per-frame [M]F..topA/B/C/wait/enc/cb/iter-end
     * traces were ~10 ms of busy-wait UART TX in the media task per frame
     * — on top of per-packet traces in h264 helper and the per-frame '*'
     * emitted from the DCMIPP ISR.  At 15 fps (66 ms/frame budget) that
     * left almost no slack for ISP/encode/send, contributing to DCMIPP
     * pipe-1 OVR events.  Now that CAMERA_FPS has been lowered to 10, we
     * have 100 ms/frame — use that budget for actual work, not tracing.
     * A coarse heartbeat every 30 frames remains for live-ness visibility. */
    uint32_t ulFrameNo = 0;
    /* Stage timing, reported in the heartbeat as avg ms/frame: tells us
     * whether the frame budget goes to VENC encode or to the network
     * send path (TLS/TURN chunked sends) without per-frame tracing.     */
    TickType_t xHbLastTick = xTaskGetTickCount();
    uint32_t   ulEncTicks  = 0U;
    uint32_t   ulSendTicks = 0U;
    while( pxCtx->ucRunning )
    {
        TickType_t xStageT0;
        TickType_t xLoopStart = xTaskGetTickCount();
        vPetWatchdog();

        /* Block on frame-event from DCMIPP ISR; returns the just-completed
         * Y+UV pair (DCMIPP has already flipped to writing the OTHER one). */
        MediaCamFrame_t xCamFrame = MediaCam_WaitFrame();

        /* Frame-content probe: confirm DCMIPP actually wrote every column.
         * Runs on the first handful of frames only so we don't flood UART
         * for the entire streaming session.                                */
        if( ulFrameNo < 10U )
        {
            mp_probe_frame_row( xCamFrame.pY );
        }

        /* ISP bookkeeping (AE/AWB) — must be called each frame             */
        MediaCam_IspUpdate();

        /* Encode from the completed (stable) NV12 pair.  VENC reads via
         * its AXI master bypassing the CPU cache — no invalidate needed. */
        xStageT0 = xTaskGetTickCount();
        lEncodedLen = MediaEnc_EncodeFrame( xCamFrame.pY,
                                            xCamFrame.pUV,
                                            pxCtx->pucEncodedFrame,
                                            ENCODED_BUF_BYTES,
                                            0 /* no forced IDR */ );
        ulEncTicks += ( uint32_t ) ( xTaskGetTickCount() - xStageT0 );

        if( lEncodedLen > 0 )
        {
            SCB_InvalidateDCache_by_Addr( ( uint32_t * ) pxCtx->pucEncodedFrame,
                                           ( int32_t ) lEncodedLen );

            xFrame.pData        = pxCtx->pucEncodedFrame;
            xFrame.size         = ( uint32_t ) lEncodedLen;
            xFrame.timestampUs  = ullTimestampUs;
            xFrame.trackKind    = TRANSCEIVER_TRACK_KIND_VIDEO;
            xFrame.freeData     = 0;   /* buffer is static — do not free   */

            if( pxCtx->pfnOnVideoFrame != NULL )
            {
                xStageT0 = xTaskGetTickCount();
                ( void ) pxCtx->pfnOnVideoFrame( pxCtx->pvVideoCtx, &xFrame );
                ulSendTicks += ( uint32_t ) ( xTaskGetTickCount() - xStageT0 );
            }

            ullTimestampUs += ulFramePeriodUs;
        }
        else
        {
            LogWarn( "[KVSMedia] H.264 encode failed (ret=%d), skipping frame.",
                     lEncodedLen );
        }

        /* Heartbeat every 30 frames (≈3s @ 10fps): frame#, encoded bytes,
         * heap, stack high-water-mark, DCMIPP OVR count.  Keeps enough
         * visibility to spot a freeze without saturating the UART in the
         * hot path.                                                        */
        if( ( ulFrameNo % 30U ) == 0U )
        {
            extern volatile uint32_t g_dcmipp_ovr_count;
            mp_raw_puts( "[M] F" ); mp_raw_dec( ( int ) ulFrameNo );
            mp_raw_puts( " len=" ); mp_raw_dec( lEncodedLen );
            mp_raw_puts( " heap=" ); mp_raw_dec( ( int ) xPortGetFreeHeapSize() );
            mp_raw_puts( " hwm=" ); mp_raw_dec( ( int ) uxTaskGetStackHighWaterMark( NULL ) );
            mp_raw_puts( " ovr=" ); mp_raw_dec( ( int ) g_dcmipp_ovr_count );
            /* Avg ms/frame since last heartbeat: encode stage, send stage,
             * and total frame period (1000/per = actual fps).             */
            {
                TickType_t xHbNow = xTaskGetTickCount();
                uint32_t ulFrames = ( ulFrameNo == 0U ) ? 1U : 30U;
                mp_raw_puts( " enc=" ); mp_raw_dec( ( int ) ( ulEncTicks / ulFrames ) );
                mp_raw_puts( " snd=" ); mp_raw_dec( ( int ) ( ulSendTicks / ulFrames ) );
                mp_raw_puts( " per=" ); mp_raw_dec( ( int ) ( ( uint32_t ) ( xHbNow - xHbLastTick ) / ulFrames ) );
                /* snd sub-stages from the H.264 send path (avg ms/frame,
                 * plus avg RTP packets/frame). */
                {
                    extern volatile uint32_t g_h264SrtpTicks;
                    extern volatile uint32_t g_h264SendTicks;
                    extern volatile uint32_t g_h264PacketCount;
                    mp_raw_puts( " srtp=" ); mp_raw_dec( ( int ) ( g_h264SrtpTicks / ulFrames ) );
                    mp_raw_puts( " isnd=" ); mp_raw_dec( ( int ) ( g_h264SendTicks / ulFrames ) );
                    mp_raw_puts( " pkt=" );  mp_raw_dec( ( int ) ( g_h264PacketCount / ulFrames ) );
                    g_h264SrtpTicks = 0U;
                    g_h264SendTicks = 0U;
                    g_h264PacketCount = 0U;
                }
                xHbLastTick = xHbNow;
                ulEncTicks = 0U;
                ulSendTicks = 0U;
            }
            mp_raw_puts( "\r\n" );
        }

        ulFrameNo++;
        ( void ) xLoopStart;   /* pacing is now handled by WaitFrame blocking */
    }

    /* Stop the DCMIPP camera pipeline + sensor before exiting so the
     * next viewer session finds the pipe in READY state and
     * is_camera_started == 0.  Without this, the pipe stays BUSY and
     * SemiPlanarDoubleBufferStart silently fails on re-entry →
     * permanent camera freeze for all subsequent viewers.              */
    MediaCam_Stop();

    LogInfo( "[KVSMedia] Media task stopped." );
    pxCtx->xTaskHandle = NULL;   /* signal completion before self-delete */
    vTaskDelete( NULL );
}

/* ── XSPI1 PSRAM (APS256XX) initialisation ─────────────────────────────── */
/*
 * The STM32N6570-DK has an AP Memory APS256XX 256-Mbit OPI PSRAM on XSPI1
 * (memory-mapped at 0x90000000).  The original project never initialised it
 * (EXTMEM_DRIVER_PSRAM = 0), so writes to .psram_bss buffers hit an
 * unconfigured AXI slave → IMPRECISERR bus faults.
 *
 * This function mirrors the BSP sequence from x-cube-n6-ai-h264-usb-uvc:
 *   1. Enable clocks & GPIOs for XSPI1 port 1 (GPIOO / GPIOP)
 *   2. HAL_XSPI_Init  — APMEM_16BITS, 256 MB, prescaler = 3 (slow start)
 *   3. Configure APS256XX mode registers (latency, x16 mode)
 *   4. Switch prescaler to 0 (bypass) for full speed
 *   5. Enable memory-mapped mode (linear-burst read + write, x16, DQS)
 */

static XSPI_HandleTypeDef hxspi_psram;

/* APS256XX commands (from aps256xx.h) */
#define APS_WRITE_REG_CMD          0xC0U
#define APS_READ_LINEAR_BURST_CMD  0x20U
#define APS_WRITE_LINEAR_BURST_CMD 0xA0U

static int prvPsramWriteReg( uint32_t ulAddr, uint8_t ucVal )
{
    XSPI_RegularCmdTypeDef xCmd = { 0 };

    xCmd.OperationType      = HAL_XSPI_OPTYPE_COMMON_CFG;
    xCmd.InstructionMode    = HAL_XSPI_INSTRUCTION_8_LINES;
    xCmd.InstructionWidth   = HAL_XSPI_INSTRUCTION_8_BITS;
    xCmd.InstructionDTRMode = HAL_XSPI_INSTRUCTION_DTR_DISABLE;
    xCmd.Instruction        = APS_WRITE_REG_CMD;
    xCmd.AddressMode        = HAL_XSPI_ADDRESS_8_LINES;
    xCmd.AddressWidth       = HAL_XSPI_ADDRESS_32_BITS;
    xCmd.AddressDTRMode     = HAL_XSPI_ADDRESS_DTR_ENABLE;
    xCmd.Address            = ulAddr;
    xCmd.AlternateBytesMode = HAL_XSPI_ALT_BYTES_NONE;
    xCmd.DataMode           = HAL_XSPI_DATA_8_LINES;
    xCmd.DataDTRMode        = HAL_XSPI_DATA_DTR_ENABLE;
    xCmd.DataLength         = 2;
    xCmd.DummyCycles        = 0;
    xCmd.DQSMode            = HAL_XSPI_DQS_DISABLE;

    if( HAL_XSPI_Command( &hxspi_psram, &xCmd, HAL_XSPI_TIMEOUT_DEFAULT_VALUE ) != HAL_OK )
        return -1;
    if( HAL_XSPI_Transmit( &hxspi_psram, &ucVal, HAL_XSPI_TIMEOUT_DEFAULT_VALUE ) != HAL_OK )
        return -1;
    return 0;
}

static int prvPsramEnableMemoryMapped( void )
{
    XSPI_RegularCmdTypeDef   xCmd  = { 0 };
    XSPI_MemoryMappedTypeDef xMmap = { 0 };

    /* Write-path command config (linear burst, x16, write-latency 7) */
    xCmd.OperationType      = HAL_XSPI_OPTYPE_WRITE_CFG;
    xCmd.InstructionMode    = HAL_XSPI_INSTRUCTION_8_LINES;
    xCmd.InstructionWidth   = HAL_XSPI_INSTRUCTION_8_BITS;
    xCmd.InstructionDTRMode = HAL_XSPI_INSTRUCTION_DTR_DISABLE;
    xCmd.Instruction        = APS_WRITE_LINEAR_BURST_CMD;
    xCmd.AddressMode        = HAL_XSPI_ADDRESS_8_LINES;
    xCmd.AddressWidth       = HAL_XSPI_ADDRESS_32_BITS;
    xCmd.AddressDTRMode     = HAL_XSPI_ADDRESS_DTR_ENABLE;
    xCmd.AlternateBytesMode = HAL_XSPI_ALT_BYTES_NONE;
    xCmd.DataMode           = HAL_XSPI_DATA_16_LINES;
    xCmd.DataDTRMode        = HAL_XSPI_DATA_DTR_ENABLE;
    xCmd.DummyCycles        = 6;   /* WriteLatency 7 → dummy = 7–1 */
    xCmd.DQSMode            = HAL_XSPI_DQS_ENABLE;

    if( HAL_XSPI_Command( &hxspi_psram, &xCmd, HAL_XSPI_TIMEOUT_DEFAULT_VALUE ) != HAL_OK )
        return -1;

    /* Read-path command config (linear burst, x16, read-latency 7) */
    xCmd.OperationType = HAL_XSPI_OPTYPE_READ_CFG;
    xCmd.Instruction   = APS_READ_LINEAR_BURST_CMD;
    xCmd.DummyCycles   = 6;   /* ReadLatency 7 → dummy = 7–1 */

    if( HAL_XSPI_Command( &hxspi_psram, &xCmd, HAL_XSPI_TIMEOUT_DEFAULT_VALUE ) != HAL_OK )
        return -1;

    /* Activate memory-mapped mode */
    xMmap.TimeOutActivation = HAL_XSPI_TIMEOUT_COUNTER_DISABLE;

    if( HAL_XSPI_MemoryMapped( &hxspi_psram, &xMmap ) != HAL_OK )
        return -1;

    return 0;
}

static void prvPsramHex32( uint32_t v )
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[11];
    buf[0] = '0'; buf[1] = 'x';
    for( int i = 7; i >= 0; i-- )
        buf[2 + (7-i)] = hex[ (v >> (i*4)) & 0xF ];
    buf[10] = 0;
    mp_raw_puts( buf );
}

static uint8_t ucPsramReady = 0U;

static void prvPsramInit( void )
{
    GPIO_InitTypeDef  xGpio  = { 0 };
    XSPIM_CfgTypeDef  xXspim = { 0 };
    uint32_t          ulClk;
    HAL_StatusTypeDef rc;

    if( ucPsramReady != 0U )
    {
        return;
    }

    mp_raw_puts( "[PSRAM] init>\r\n" );

    /* ── 1. Power & clocks ─────────────────────────────────────────────── */
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWREx_EnableVddIO2();
    HAL_PWREx_ConfigVddIORange( PWR_VDDIO2, PWR_VDDIO_RANGE_1V8 );

    __HAL_RCC_XSPI1_CLK_ENABLE();
    __HAL_RCC_XSPI1_FORCE_RESET();
    __HAL_RCC_XSPI1_RELEASE_RESET();

    __HAL_RCC_XSPIM_CLK_ENABLE();
    /* Do NOT reset XSPIM — it is shared with XSPI2 (NOR flash) */

    __HAL_RCC_GPIOO_CLK_ENABLE();
    __HAL_RCC_GPIOP_CLK_ENABLE();

    mp_raw_puts( "[PSRAM] clk OK\r\n" );

    /* ── 2. GPIO — GPIOO: CS(0), DQS0(2), DQS1(3), CLK(4) ────────────── */
    xGpio.Mode      = GPIO_MODE_AF_PP;
    xGpio.Pull      = GPIO_PULLUP;
    xGpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    xGpio.Alternate = GPIO_AF9_XSPIM_P1;

    xGpio.Pin = GPIO_PIN_0;
    HAL_GPIO_Init( GPIOO, &xGpio );
    xGpio.Pin = GPIO_PIN_2;
    HAL_GPIO_Init( GPIOO, &xGpio );
    xGpio.Pin = GPIO_PIN_3;
    HAL_GPIO_Init( GPIOO, &xGpio );
    xGpio.Pin = GPIO_PIN_4;
    HAL_GPIO_Init( GPIOO, &xGpio );

    /* GPIOP: D0..D15 (pins 0‥15) */
    xGpio.Pin = GPIO_PIN_0  | GPIO_PIN_1  | GPIO_PIN_2  | GPIO_PIN_3  |
                GPIO_PIN_4  | GPIO_PIN_5  | GPIO_PIN_6  | GPIO_PIN_7  |
                GPIO_PIN_8  | GPIO_PIN_9  | GPIO_PIN_10 | GPIO_PIN_11 |
                GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
    HAL_GPIO_Init( GPIOP, &xGpio );

    mp_raw_puts( "[PSRAM] gpio OK\r\n" );

    /* ── 3. XSPI1 peripheral init (prescaler = 3 for register config) ── */
    ulClk = HAL_RCCEx_GetPeriphCLKFreq( RCC_PERIPHCLK_XSPI1 );
    mp_raw_puts( "[PSRAM] clkHz=" ); prvPsramHex32( ulClk ); mp_raw_puts( "\r\n" );

    hxspi_psram.Instance                       = XSPI1;
    hxspi_psram.Init.FifoThresholdByte         = 8;
    hxspi_psram.Init.MemoryType                = HAL_XSPI_MEMTYPE_APMEM_16BITS;
    hxspi_psram.Init.MemoryMode                = HAL_XSPI_SINGLE_MEM;
    hxspi_psram.Init.MemorySize                = HAL_XSPI_SIZE_256MB;
    hxspi_psram.Init.MemorySelect              = HAL_XSPI_CSSEL_NCS1;
    hxspi_psram.Init.ChipSelectHighTimeCycle    = 5;
    hxspi_psram.Init.ClockMode                 = HAL_XSPI_CLOCK_MODE_0;
    hxspi_psram.Init.ClockPrescaler            = 3;
    hxspi_psram.Init.SampleShifting            = HAL_XSPI_SAMPLE_SHIFT_NONE;
    hxspi_psram.Init.DelayHoldQuarterCycle     = HAL_XSPI_DHQC_ENABLE;
    hxspi_psram.Init.ChipSelectBoundary        = HAL_XSPI_BONDARYOF_16KB;
    hxspi_psram.Init.FreeRunningClock          = HAL_XSPI_FREERUNCLK_DISABLE;
    hxspi_psram.Init.WrapSize                  = HAL_XSPI_WRAP_NOT_SUPPORTED;
    hxspi_psram.Init.MaxTran                   = 0;
    hxspi_psram.Init.Refresh                   = ( ulClk > 0 )
                                                  ? ( ( 2U * ( ulClk / 3U ) ) / 1000000U ) - 4U
                                                  : 0;

    rc = HAL_XSPI_Init( &hxspi_psram );
    mp_raw_puts( "[PSRAM] HAL_XSPI_Init=" ); prvPsramHex32( rc ); mp_raw_puts( "\r\n" );
    if( rc != HAL_OK ) { mp_raw_puts( "[PSRAM] FAIL init\r\n" ); return; }

    /* ── 4. XSPI Manager — route to I/O port 1 ────────────────────────── */
    xXspim.nCSOverride = HAL_XSPI_CSSEL_OVR_NCS1;
    xXspim.IOPort      = HAL_XSPIM_IOPORT_1;
    xXspim.Req2AckTime = 1;
    rc = HAL_XSPIM_Config( &hxspi_psram, &xXspim, HAL_XSPI_TIMEOUT_DEFAULT_VALUE );
    mp_raw_puts( "[PSRAM] XSPIM_Config=" ); prvPsramHex32( rc ); mp_raw_puts( "\r\n" );
    if( rc != HAL_OK ) { mp_raw_puts( "[PSRAM] FAIL xspim\r\n" ); return; }

    /* ── 5. APS256XX mode-register config (slow clock) ─────────────────── */
    int wr;
    wr = prvPsramWriteReg( 0, 0x30 );  /* MR0: Read Latency = 7 */
    mp_raw_puts( "[PSRAM] MR0=" ); prvPsramHex32( wr ); mp_raw_puts( "\r\n" );
    wr = prvPsramWriteReg( 4, 0x20 );  /* MR4: Write Latency = 7 */
    mp_raw_puts( "[PSRAM] MR4=" ); prvPsramHex32( wr ); mp_raw_puts( "\r\n" );
    wr = prvPsramWriteReg( 8, 0x40 );  /* MR8: switch to x16 mode */
    mp_raw_puts( "[PSRAM] MR8=" ); prvPsramHex32( wr ); mp_raw_puts( "\r\n" );

    /* ── 6. Bypass prescaler for full-speed operation ──────────────────── */
    rc = HAL_XSPI_SetClockPrescaler( &hxspi_psram, 0 );
    mp_raw_puts( "[PSRAM] prescaler=" ); prvPsramHex32( rc ); mp_raw_puts( "\r\n" );

    /* ── 7. Enable memory-mapped mode (linear-burst read+write, x16) ─── */
    int mrc = prvPsramEnableMemoryMapped();
    mp_raw_puts( "[PSRAM] mmap=" ); prvPsramHex32( mrc ); mp_raw_puts( "\r\n" );

    if( mrc == 0 )
    {
        /* Quick smoke-test: write/read a word at the PSRAM base */
        volatile uint32_t *pTest = (volatile uint32_t *)0x90000000UL;
        *pTest = 0xDEADBEEFUL;
        __DSB();
        uint32_t v = *pTest;
        mp_raw_puts( "[PSRAM] test=" ); prvPsramHex32( v ); mp_raw_puts( "\r\n" );
    }

    mp_raw_puts( "[PSRAM] init<\r\n" );
    ucPsramReady = 1U;
}

/* Callable from outside the media source: data now lives in .psram_bss
 * (xAppContext, AI buffers) that is touched before AppMediaSourcePort_Init
 * runs, so callers must be able to bring PSRAM up first.  Idempotent. */
void MediaPort_EnsurePsram( void )
{
    prvPsramInit();
}

/* ── Public AppMediaSourcePort_* API ────────────────────────────────────── */

int32_t AppMediaSourcePort_Init( void )
{
    int lWidth, lHeight;

    memset( &xMediaCtx, 0, sizeof( xMediaCtx ) );

    /* Initialise XSPI1 → APS256XX PSRAM in memory-mapped mode before any
     * access to the .psram_bss buffers (frame buffers + VENC allocator).   */
    vPetWatchdog();
    prvPsramInit();

    /* Use static PSRAM buffers (placed in .psram_bss by linker)            */
    xMediaCtx.pucEncodedFrame = ucEncodedFrameBuf;

    /* Camera pipeline init — detects sensor, configures DCMIPP PIPE1      */
    vPetWatchdog();
    mp_raw_puts( "[MP] cam init>\r\n" );
    MediaCam_Init();
    mp_raw_puts( "[MP] cam init<\r\n" );

    vPetWatchdog();
    lWidth  = MediaCam_GetWidth();
    lHeight = MediaCam_GetHeight();
    mp_raw_puts( "[MP] enc init>\r\n" );

    /* Encoder init */
    vPetWatchdog();
    MediaEncConf_t xEncConf = { .lWidth = lWidth, .lHeight = lHeight, .lFps = CAMERA_FPS };
    MediaEnc_Init( &xEncConf );
    vPetWatchdog();
    mp_raw_puts( "[MP] enc init<\r\n" );

    mp_raw_puts( "[MP] READY\r\n" );
    return 0;
}

int32_t AppMediaSourcePort_Start( OnFrameReadyToSend_t pfnOnVideoFrame,
                                  void *               pvVideoCtx,
                                  OnFrameReadyToSend_t pfnOnAudioFrame,
                                  void *               pvAudioCtx )
{
    ( void ) pfnOnAudioFrame;   /* No audio on this board */
    ( void ) pvAudioCtx;

    xMediaCtx.pfnOnVideoFrame = pfnOnVideoFrame;
    xMediaCtx.pvVideoCtx      = pvVideoCtx;
    xMediaCtx.ucRunning       = 1U;

    if( xTaskCreate( prvMediaTask,
                     "KVSMedia",
                     MEDIA_TASK_STACK_DEPTH,
                     &xMediaCtx,
                     MEDIA_TASK_PRIORITY,
                     &xMediaCtx.xTaskHandle ) != pdPASS )
    {
        LogError( "[KVSMedia] Failed to create media task." );
        return -1;
    }

    return 0;
}

void AppMediaSourcePort_Stop( void )
{
    mp_raw_puts( "[MP] Stop> ucRun=" );
    mp_raw_dec( ( int ) xMediaCtx.ucRunning );
    mp_raw_puts( " hdl=" );
    mp_raw_dec( xMediaCtx.xTaskHandle != NULL );
    mp_raw_puts( "\r\n" );

    xMediaCtx.ucRunning = 0U;

    /* Wait for the media task to finish its cleanup (MediaCam_Stop) and
     * self-delete.  WaitFrame has a 2 s timeout, so worst case the task
     * takes ~2 s + ~1 s (DCMIPP stop timeout) to exit.  Polling at 100 ms
     * keeps responsiveness without burning CPU.  Without this wait, the
     * next AppMediaSourcePort_Start() can race: two media tasks touching
     * the same DCMIPP hardware simultaneously.                            */
    while( xMediaCtx.xTaskHandle != NULL )
    {
        vTaskDelay( pdMS_TO_TICKS( 100 ) );
    }

    mp_raw_puts( "[MP] Stop<\r\n" );
}

void AppMediaSourcePort_Destroy( void )
{
    MediaEnc_DeInit();

    /* Frame buffers are static arrays in .psram_bss — nothing to free */
    xMediaCtx.pucEncodedFrame = NULL;
}

void AppMediaSourcePort_PlayAudioFrame( MediaFrame_t * pxFrame )
{
    /* No audio output capability on this board */
    ( void ) pxFrame;
}
