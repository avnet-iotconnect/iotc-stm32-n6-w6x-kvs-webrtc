/*
 * STM32N6570 KVS WebRTC port — media_enc.c
 *
 * STM32N6 hardware H.264 encoder for the KVS WebRTC media pipeline.
 * Derived from x-cube-n6-ai-h264-usb-uvc/Src/app_enc.c with renaming
 * and removal of the JPEG encoder path.
 *
 * Copyright (c) 2023 STMicroelectronics.  All rights reserved.
 * SPDX-License-Identifier: LicenseRef-STMicroelectronics
 */

#include "media_enc.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "h264encapi.h"
#include "stm32n6xx_hal.h"
#include "ewl.h"

/* Internal allocator for encoder HW buffers (placed in PSRAM) */
#define VENC_ALLOCATOR_SIZE     ( 4U * 1024U * 1024U )
/* RATE_CTRL_QP_DEFAULT raised 25 -> 40.  This is the starting QP for the
 * first I-frame (before VBR rate-control converges).  At QP 25 the first
 * I-frame was ~320 KB for 720p, which saturated PSRAM/AXI bandwidth shared
 * with DCMIPP+VENC+lwIP and stalled the CPU mid-burst.  QP 40 produces
 * dramatically smaller I-frames (~20-30 KB) and lets the rate-control
 * settle into the 500 kbps envelope from the very first frame.            */
#define RATE_CTRL_QP_DEFAULT    40

/* Linker section attributes — keep ALIGN_32 / IN_PSRAM from the original */
#if defined ( __GNUC__ )
    #define ALIGN_32    __attribute__((aligned(32)))
    #define IN_PSRAM    __attribute__((section(".psram_bss")))
#else
    #define ALIGN_32
    #define IN_PSRAM
#endif

static uint8_t ucVencAllocatorBuffer[ VENC_ALLOCATOR_SIZE ] ALIGN_32 IN_PSRAM;
static uint8_t * pucVencAllocatorPos = ucVencAllocatorBuffer;

/* ── Raw UART debug (bypasses FreeRTOS logging) ────────────────────────── */
/* Bounded spin — see the detailed rationale in kvs_webrtc_task.c.         */
extern void vPetWatchdog( void );
static inline void venc_raw_putc( char c )
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
static void venc_raw_puts( const char *s ) { while( *s ) venc_raw_putc( *s++ ); }
static void venc_raw_dec( int v )
{
    char buf[ 12 ];
    int  n = 0;
    if( v < 0 ) { venc_raw_putc( '-' ); v = -v; }
    if( v == 0 ) { venc_raw_putc( '0' ); return; }
    while( v > 0 ) { buf[ n++ ] = '0' + ( v % 10 ); v /= 10; }
    while( n-- ) venc_raw_putc( buf[ n ] );
}

typedef struct
{
    H264EncInst xHdl;
    int         lIsSpsPpsDone;
    uint64_t    ullPicCnt;
    int         lGopLen;
} VencContext_t;

static VencContext_t xVencInstance;

/* ── Rate control helpers ───────────────────────────────────────────────── */

static void prvSetupVbr( H264EncRateCtrl * pxRate,
                         int               lBitrate,
                         int               lGopLen,
                         int               lQp )
{
    pxRate->pictureRc    = 1;
    pxRate->mbRc         = 1;
    pxRate->pictureSkip  = 0;
    pxRate->hrd          = 0;
    pxRate->qpHdr        = lQp;
    pxRate->qpMin        = 10;
    pxRate->qpMax        = 51;
    pxRate->gopLen       = lGopLen;
    pxRate->bitPerSecond = lBitrate;
    pxRate->intraQpDelta = 0;
}

/* ── Encoding helpers ───────────────────────────────────────────────────── */

static int prvEncodeStart( VencContext_t * pxCtx,
                           uint8_t *       pOut,
                           size_t          xOutLen,
                           size_t *        pxOutLen )
{
    H264EncOut xEncOut;
    H264EncIn  xEncIn;
    size_t     xStartLen;
    int        ret;

    xEncIn.pOutBuf    = ( u32 * ) pOut;
    xEncIn.busOutBuf  = ( ptr_t ) pOut;
    xEncIn.outBufSize = xOutLen;

    venc_raw_puts( "[V] strmStart>\r\n" );
    ret = H264EncStrmStart( pxCtx->xHdl, &xEncIn, &xEncOut );
    venc_raw_puts( "[V] strmStart<\r\n" );
    if( ret )
        return ret;

    xStartLen = xEncOut.streamSize;

    /* Align output to 8-byte boundary with a padding NAL if necessary */
    {
        uint32_t ulAddr   = ( uint32_t )( pOut + xStartLen );
        int      lPadSize = 8 - ( ulAddr % 8 );
        int      lPadLen  = 0;

        if( ( ulAddr % 8 ) != 0 )
        {
            if( lPadSize < 6 )
                lPadSize += 8;

            if( ( size_t ) lPadSize <= ( xOutLen - xStartLen ) )
            {
                uint8_t * pPad = pOut + xStartLen;
                pPad[ lPadLen++ ] = 0x00;
                pPad[ lPadLen++ ] = 0x00;
                pPad[ lPadLen++ ] = 0x00;
                pPad[ lPadLen++ ] = 0x01;
                pPad[ lPadLen++ ] = 0x2c;
                lPadSize -= 5;
                while( lPadSize-- )
                    pPad[ lPadLen++ ] = 0xff;
            }
            xStartLen += lPadLen;
        }
    }

    *pxOutLen = xStartLen;
    return 0;
}

static int prvEncodeFrame( VencContext_t * pxCtx,
                           uint8_t *       pInY,
                           uint8_t *       pInUV,
                           uint8_t *       pOut,
                           size_t          xOutLen,
                           size_t *        pxOutLen,
                           int             lIntraForce )
{
    H264EncOut xEncOut;
    H264EncIn  xEncIn;
    int        ret;

    /* NV12 semi-planar: busLuma points to the Y-plane, busChromaU points
     * to the interleaved UV-plane.  busChromaV is unused for semiplanar.   */
    xEncIn.busLuma        = ( ptr_t ) pInY;
    xEncIn.busChromaU     = ( ptr_t ) pInUV;
    xEncIn.busChromaV     = 0;
    xEncIn.pOutBuf        = ( u32 * ) pOut;
    xEncIn.busOutBuf      = ( ptr_t ) pOut;
    xEncIn.outBufSize     = xOutLen;
    xEncIn.codingType     = ( pxCtx->ullPicCnt % ( pxCtx->lGopLen + 1 ) == 0 )
                            ? H264ENC_INTRA_FRAME : H264ENC_PREDICTED_FRAME;
    xEncIn.codingType     = lIntraForce ? H264ENC_INTRA_FRAME : xEncIn.codingType;
    xEncIn.timeIncrement  = 1;
    xEncIn.ipf            = H264ENC_REFERENCE_AND_REFRESH;
    xEncIn.ltrf           = H264ENC_NO_REFERENCE_NO_REFRESH;
    xEncIn.lineBufWrCnt   = 0;
    xEncIn.sendAUD        = 0;

    venc_raw_puts( ( xEncIn.codingType == H264ENC_INTRA_FRAME )
                   ? "[V] strmEnc>I\r\n" : "[V] strmEnc>P\r\n" );
    ret = H264EncStrmEncode( pxCtx->xHdl, &xEncIn, &xEncOut, NULL, NULL, NULL );
    venc_raw_puts( "[V] strmEnc<r=" );
    venc_raw_dec( ret );
    venc_raw_puts( "\r\n" );
    if( ret != H264ENC_FRAME_READY )
        return ret;   /* propagate actual H264EncRet error code */

    pxCtx->ullPicCnt++;
    *pxOutLen = xEncOut.streamSize;
    return 0;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void MediaEnc_Init( const MediaEncConf_t * pxConf )
{
    VencContext_t *      pxCtx = &xVencInstance;
    H264EncConfig        xConfig;
    H264EncPreProcessingCfg xPpCfg;
    H264EncCodingCtrl    xCtrl;
    H264EncRateCtrl      xRate;
    int                  lTargetBitrate;
    int                  ret;

    /* Enable VENC hardware clocks — matches LL_VENC_Init() sequence:
     * 1. APB5 bus clock
     * 2. VENCRAM memory clock
     * 3. VENC peripheral clock
     * A brief delay after clock enable lets the VENC registers stabilise.    */
    WRITE_REG( RCC->BUSENSR, RCC_BUSENSR_APB5ENS );
    __HAL_RCC_VENCRAM_MEM_CLK_ENABLE();
    __HAL_RCC_VENC_CLK_ENABLE();
    /* DSB + small delay so clock propagates before first register access */
    __DSB();
    for( volatile int i = 0; i < 1000; i++ ) {}

    /* Suppress HardFault logging during VENC init — the hardware triggers
     * hundreds of imprecise bus faults while registers stabilise. */
    extern volatile uint8_t g_hardfault_log_suppress;
    g_hardfault_log_suppress = 1;

    memset( &xConfig, 0, sizeof( xConfig ) );
    /* GOP length extended from (lFps-1) ≈ 1 s of P-frames between I-frames
     * up to (lFps*5-1) ≈ 5 s between I-frames.  The previous 1 s cadence
     * produced a bursty AXI/PSRAM load: the big I-frame encode is an order
     * of magnitude heavier (reads + writes + VENC internal search state)
     * than a P-frame, and one every second meant the bus was periodically
     * saturated by the I-frame burst colliding with DCMIPP writes and
     * Wi-Fi TX.  Viewer-side receivers handle longer GOPs fine: they use
     * RTCP PLI/FIR feedback to request an IDR on loss (lIntraForce path). */
    pxCtx->lGopLen          = pxConf->lFps * 5 - 1;
    xConfig.streamType      = H264ENC_BYTE_STREAM;
    xConfig.viewMode        = H264ENC_BASE_VIEW_SINGLE_BUFFER;
    xConfig.level           = H264ENC_LEVEL_5_1;
    xConfig.width           = pxConf->lWidth;
    xConfig.height          = pxConf->lHeight;
    xConfig.frameRateNum    = pxConf->lFps;
    xConfig.frameRateDenom  = 1;
    xConfig.refFrameAmount  = 1;

    ret = H264EncInit( &xConfig, &pxCtx->xHdl );
    assert( ret == H264ENC_OK );

    ret = H264EncGetPreProcessing( pxCtx->xHdl, &xPpCfg );
    assert( ret == H264ENC_OK );
    /* NV12 (YUV420 semi-planar).  DCMIPP PIPE1 pixel packer does the
     * RGB→YUV conversion (P1YUVCR) before writing the Y plane to AR1
     * and the interleaved UV plane to P1PPM1AR1.  VENC reads both
     * directly; no internal conversion step, and only 12 bpp of PSRAM
     * bandwidth vs 32 bpp for ARGB8888/RGB888.                           */
    xPpCfg.inputType = H264ENC_YUV420_SEMIPLANAR;
    ret = H264EncSetPreProcessing( pxCtx->xHdl, &xPpCfg );
    assert( ret == H264ENC_OK );

    ret = H264EncGetCodingCtrl( pxCtx->xHdl, &xCtrl );
    assert( ret == H264ENC_OK );
    xCtrl.idrHeader = 1;
    ret = H264EncSetCodingCtrl( pxCtx->xHdl, &xCtrl );
    assert( ret == H264ENC_OK );

    ret = H264EncGetRateCtrl( pxCtx->xHdl, &xRate );
    assert( ret == H264ENC_OK );
    /* Target bitrate slashed from (W*H*12)*fps/30 (= 5.5 Mbps for 720p15)
     * to 500 kbps.  At 5.5 Mbps the encoder produced ~358 KB I-frames which
     * (a) saturated PSRAM/AXI bandwidth shared with DCMIPP+VENC+lwIP, causing
     * mid-burst CPU stalls partway through transmit, and (b) far exceeded the
     * sustainable upload rate over TURN-TLS.  500 kbps / 15 fps -> ~33 KB
     * average frame, ~80 KB I-frames — fits cleanly inside the bandwidth
     * envelope of both PSRAM and the network path.
     *
     * Tier-1 quality bump: raised to 1.5 Mbps now that turns:tcp relay path is
     * verified stable on external power.  At 640x480/10fps this yields
     * ~150 KB avg + ~240 KB I-frames — still well under the 5.5 Mbps ceiling
     * that previously caused the mid-burst stalls, while giving a visible
     * quality improvement in the IoTConnect viewer.
     *
     * 2026-04-14: dropped to 300 kbps.  With the W6x WAN UDP blackhole
     * forcing relay over TURN+TLS+TCP, the log showed ~1 FPS effective
     * throughput at 1.5 Mbps: large P-frames (~18 KB) and I-frames
     * (40–80 KB) were holding the socket mutex past its 500 ms timeout
     * window, dropping RTCP reports and RX packets.  At 300 kbps/5 fps
     * the average frame drops to ~7.5 KB — each TLS send completes well
     * inside the mutex window.
     *
     * 2026-04-14 (bump): raised to 800 kbps.  Live streaming at 300 kbps /
     * 10 fps produced P-frames of only ~200-600 B (rate controller was
     * biased way below budget on static scenes).  Plenty of headroom in
     * the TURN+TCP pipe, so push quality up: 800 kbps / 10 fps targets
     * ~10 KB average P-frames and ~30-50 KB I-frames — each TLS send
     * still completes comfortably inside the 500 ms mutex window, but
     * motion scenes get visibly sharper encoding.
     *
     * 2026-04-16: raised to 2 Mbps.  V1.3.0 middleware fixed WAN UDP RX,
     * so ICE now negotiates UDP TURN relay instead of TCP.  UDP eliminates
     * TLS framing overhead, TCP head-of-line blocking, and the socket-mutex
     * contention that throttled TCP relay to ~1 FPS at high bitrates.
     * At 640x480/10fps, 2 Mbps targets ~25 KB average P-frames and
     * ~80 KB I-frames — well within PSRAM/AXI headroom at this resolution.
     *
     * 2026-04-16: trimmed to 1.5 Mbps.  At 2 Mbps / 15 fps I-frames grew
     * to 45-52 KB, fragmenting into ~37 UDP TURN packets and stalling the
     * send path past the 500 ms socket-mutex window (RTCP timeouts).
     * 1.5 Mbps / 15 fps targets ~12 KB P-frames and ~30 KB I-frames —
     * comfortably inside the mutex budget while still a large quality
     * improvement over the previous 800 kbps TCP path.
     *
     * 2026-04-16: trimmed to 1.0 Mbps.  At 1.5 Mbps I-frames still hit
     * ~40 KB, exceeding the 500 ms socket-mutex window over UDP TURN.
     * 1.0 Mbps / 15 fps targets ~8 KB P-frames and ~25 KB I-frames.      */
    lTargetBitrate = 1000000;
    prvSetupVbr( &xRate, lTargetBitrate, pxConf->lFps, RATE_CTRL_QP_DEFAULT );
    ret = H264EncSetRateCtrl( pxCtx->xHdl, &xRate );
    assert( ret == H264ENC_OK );

    g_hardfault_log_suppress = 0;
}

void MediaEnc_DeInit( void )
{
    int ret = H264EncRelease( xVencInstance.xHdl );
    assert( ret == H264ENC_OK );
}

int MediaEnc_EncodeFrame( uint8_t * pInY,
                          uint8_t * pInUV,
                          uint8_t * pOut,
                          size_t    xOutLen,
                          int       lIntraForce )
{
    VencContext_t * pxCtx    = &xVencInstance;
    size_t          xStartLen = 0;
    size_t          xFrameLen;
    int             ret;

    if( !pxCtx->lIsSpsPpsDone )
    {
        ret = prvEncodeStart( pxCtx, pOut, xOutLen, &xStartLen );
        if( ret )
            return -1;
        pxCtx->lIsSpsPpsDone = 1;
    }

    ret = prvEncodeFrame( pxCtx, pInY, pInUV,
                          &pOut[ xStartLen ], xOutLen - xStartLen,
                          &xFrameLen, lIntraForce );
    if( ret )
        return ret;   /* propagate H264EncRet (negative enum) */

    return ( int )( xStartLen + xFrameLen );
}

/* ── EWL allocator stubs (required by VideoEncoder library) ─────────────── */

void * EWLmalloc( u32 n )
{
    void * p = malloc( n );
    assert( p );
    return p;
}

void EWLfree( void * p )
{
    free( p );
}

void * EWLcalloc( u32 n, u32 s )
{
    void * p = calloc( n, s );
    assert( p );
    return p;
}

i32 EWLMallocLinear( const void * instance, u32 size, EWLLinearMem_t * info )
{
    ( void ) instance;

    if( pucVencAllocatorPos + size > ucVencAllocatorBuffer + VENC_ALLOCATOR_SIZE )
        return -1;

    info->size            = size;
    info->virtualAddress  = ( u32 * ) pucVencAllocatorPos;
    info->busAddress      = ( ptr_t ) info->virtualAddress;
    pucVencAllocatorPos  += size;
    return 0;
}
