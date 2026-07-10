/*
 * STM32N6570 KVS WebRTC port — media_cam.c
 *
 * DCMIPP camera initialisation for the KVS WebRTC media pipeline.
 * Derived from x-cube-n6-ai-h264-usb-uvc/Src/app_cam.c with the
 * AI/NPU (PIPE2) and USB-UVC output paths removed.
 *
 * Copyright (c) 2023 STMicroelectronics.  All rights reserved.
 * SPDX-License-Identifier: LicenseRef-STMicroelectronics
 */

#include <assert.h>
#include "media_cam.h"
#include "media_cam_config.h"   /* sensor / venc dimension macros          */
#include "cmw_camera.h"
#include "isp_core.h"            /* ISP_SensorInfoTypeDef                   */
#include "FreeRTOS.h"            /* vPetWatchdog() declared in FreeRTOSConfig.h */
#include "task.h"                /* vTaskDelay for post-start poll loop     */
#include "semphr.h"              /* binary sem for frame-ready signaling    */
#include "stm32n6570_discovery_bus.h"  /* BSP_I2C1_ReadReg16 for sensor probe */

#include "logging_levels.h"
#define LOG_LEVEL   LOG_INFO
#include "logging.h"

/* ── Raw UART debug (bypasses FreeRTOS logging) ────────────────────────── */
/* Bounded spin — see rationale in kvs_webrtc_task.c. */
extern void vPetWatchdog( void );
static inline void raw_putc( char c )
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
static void raw_puts( const char *s ) { while( *s ) raw_putc( *s++ ); }
static void raw_hex32( uint32_t v )
{
    static const char hex[] = "0123456789ABCDEF";
    raw_putc( '0' ); raw_putc( 'x' );
    for( int i = 28; i >= 0; i -= 4 )
        raw_putc( hex[ ( v >> i ) & 0xF ] );
}
static void raw_dec( uint32_t v )
{
    char buf[ 11 ];
    int  n = 0;
    if( v == 0 ) { raw_putc( '0' ); return; }
    while( v > 0 ) { buf[ n++ ] = '0' + ( v % 10 ); v /= 10; }
    while( n-- ) raw_putc( buf[ n ] );
}

/* ── Private state ──────────────────────────────────────────────────────── */
static int xSensorWidth;
static int xSensorHeight;
static int xVencWidth;
static int xVencHeight;
static int xSensorMirrorFlip = CMW_MIRRORFLIP_NONE;

/* ── Double-buffer ping-pong state ─────────────────────────────────────── */
/*
 * xFrameReadySem is a binary semaphore "given" from the DCMIPP frame-event
 * ISR (via CMW_CAMERA_PIPE_FrameEventCallback override below) whenever a
 * full frame has just been written.  The media task blocks on it in
 * MediaCam_WaitFrame().
 *
 * xPingPongY[] / xPingPongUV[] record the Y-plane and UV-plane pointers
 * for each of the two DCMIPP ping-pong slots.  ulFrameCount is incremented
 * on each frame-event; since DCMIPP DBM starts by writing slot 0 and
 * alternates thereafter:
 *    ulFrameCount == 1 → slot 0 just completed
 *    ulFrameCount == 2 → slot 1 just completed
 *    ...
 * So the just-completed (Y, UV) pair is at index ((ulFrameCount - 1) & 1).
 */
static SemaphoreHandle_t xFrameReadySem     = NULL;
static uint8_t *         pucPingPongY[ 2 ]  = { NULL, NULL };
static uint8_t *         pucPingPongUV[ 2 ] = { NULL, NULL };
static volatile uint32_t ulFrameEventCount   = 0;

/* ── Internal helpers ───────────────────────────────────────────────────── */

static void prvSetSensorInfo( CMW_Sensor_Name_t eSensor )
{
    switch( eSensor )
    {
        case CMW_VD66GY_Sensor:
            xSensorWidth      = SENSOR_VD66GY_WIDTH;
            xSensorHeight     = SENSOR_VD66GY_HEIGHT;
            xSensorMirrorFlip = SENSOR_VD66GY_FLIP;
            xVencWidth        = VENC_VD66GY_WIDTH;
            xVencHeight       = VENC_VD66GY_HEIGHT;
            break;
        case CMW_IMX335_Sensor:
            xSensorWidth      = SENSOR_IMX335_WIDTH;
            xSensorHeight     = SENSOR_IMX335_HEIGHT;
            xSensorMirrorFlip = SENSOR_IMX335_FLIP;
            xVencWidth        = VENC_IMX335_WIDTH;
            xVencHeight       = VENC_IMX335_HEIGHT;
            break;
        case CMW_VD55G1_Sensor:
            xSensorWidth      = SENSOR_VD55G1_WIDTH;
            xSensorHeight     = SENSOR_VD55G1_HEIGHT;
            xSensorMirrorFlip = SENSOR_VD55G1_FLIP;
            xVencWidth        = VENC_VD55G1_WIDTH;
            xVencHeight       = VENC_VD55G1_HEIGHT;
            break;
        case CMW_VD1943_Sensor:
            xSensorWidth      = SENSOR_VD1943_WIDTH;
            xSensorHeight     = SENSOR_VD1943_HEIGHT;
            xSensorMirrorFlip = SENSOR_VD1943_FLIP;
            xVencWidth        = VENC_VD1943_WIDTH;
            xVencHeight       = VENC_VD1943_HEIGHT;
            break;
        default:
            assert( 0 );
    }
}

static void prvInitCropConfig( CMW_Manual_roi_area_t * pxRoi,
                               int lSensorW,
                               int lSensorH )
{
#define MIN( a, b )  ( ( (a) < (b) ) ? (a) : (b) )
    const float fRatioX = ( float ) lSensorW / xVencWidth;
    const float fRatioY = ( float ) lSensorH / xVencHeight;
    const float fRatio  = MIN( fRatioX, fRatioY );

    assert( fRatio >= 1 );
    assert( fRatio < 64 );

    pxRoi->width    = ( uint32_t ) MIN( xVencWidth  * fRatio, lSensorW );
    pxRoi->height   = ( uint32_t ) MIN( xVencHeight * fRatio, lSensorH );
    pxRoi->offset_x = ( lSensorW - ( int ) pxRoi->width  + 1 ) / 2;
    pxRoi->offset_y = ( lSensorH - ( int ) pxRoi->height + 1 ) / 2;
#undef MIN
}

static void prvDCMIPP_PipeInitEncoder( int lSensorW, int lSensorH )
{
    CMW_DCMIPP_Conf_t xConf;
    uint32_t ulPitch;
    int ret;

    xConf.output_width           = xVencWidth;
    xConf.output_height          = xVencHeight;
    xConf.output_format          = CAPTURE_FORMAT;
    xConf.output_bpp             = CAPTURE_BPP;
    xConf.mode                   = CMW_Aspect_ratio_manual_roi;
    xConf.enable_swap            = 0;
    xConf.enable_gamma_conversion = 0;
    prvInitCropConfig( &xConf.manual_conf, lSensorW, lSensorH );

    ret = CMW_CAMERA_SetPipeConfig( DCMIPP_PIPE1, &xConf, &ulPitch );
    assert( ret == HAL_OK );
    assert( ulPitch == ( uint32_t )( xConf.output_width * xConf.output_bpp ) );
}

static void prvDCMIPP_IpPlugInit( DCMIPP_HandleTypeDef * pxHdcmipp )
{
    DCMIPP_IPPlugConfTypeDef xIpPlugConf = { 0 };
    int ret;

    /* DCMIPP IpPlug config — hybrid of reference and PSRAM-friendly backoff.
     *
     * DPREG PARTITION (matches x-cube-n6-ai-h264-usb-uvc reference):
     * Even though only PIPE1/CLIENT5 is actually streaming, CLIENT2 must
     * still be configured so the DPREG (data prefetch register) buffer
     * space is explicitly partitioned.  When CLIENT2 is left at its
     * default/reset range, it overlaps CLIENT5's range and the DCMIPP
     * produces exactly one good frame then stops reloading.  Symptom seen
     * prior: F0=37KB I-frame followed by ~46-byte "skip-all-macroblocks"
     * P-frames, viewer showed "frozen" stripes.
     *
     * We give CLIENT5 the LARGE share of DPREG since it's the only pipe
     * actually running, and CLIENT2 gets a minimal 80-page slice.  This
     * is opposite of the reference (which runs CLIENT2/PIPE2 as the NN
     * pipe at higher data rate than the display pipe).
     *
     * BURST / OUTSTANDING (PSRAM-friendly, NOT matching reference):
     * Reference uses BURST_128 / OUTSTANDING_3 on CLIENT5.  That works in
     * the reference (USB device, no lwIP) but in our port VENC reads +
     * DCMIPP writes + Wi-Fi TX all contend for XSPI1/PSRAM bandwidth.
     * BURST_128 starved Wi-Fi TX pbuf allocation → lwIP pbuf_alloc hung
     * in the RTP packetizer ~14 packets into the first I-frame ("video
     * is black" because viewer never received a complete I-frame).
     * Keeping BURST_64 / OUTSTANDING_2 preserves headroom for the
     * network stack while still allowing enough DCMIPP throughput.       */
    xIpPlugConf.MemoryPageSize = DCMIPP_MEMORY_PAGE_SIZE_256BYTES;

    xIpPlugConf.Client                     = DCMIPP_CLIENT2;
    xIpPlugConf.Traffic                    = DCMIPP_TRAFFIC_BURST_SIZE_64BYTES;
    xIpPlugConf.MaxOutstandingTransactions = DCMIPP_OUTSTANDING_TRANSACTION_NONE;
    xIpPlugConf.DPREGStart                 = 0;
    xIpPlugConf.DPREGEnd                   = 79;
    xIpPlugConf.WLRURatio                  = 0;
    ret = HAL_DCMIPP_SetIPPlugConfig( pxHdcmipp, &xIpPlugConf );
    assert( ret == HAL_OK );

    xIpPlugConf.Client                     = DCMIPP_CLIENT5;
    xIpPlugConf.Traffic                    = DCMIPP_TRAFFIC_BURST_SIZE_64BYTES;
    xIpPlugConf.MaxOutstandingTransactions = DCMIPP_OUTSTANDING_TRANSACTION_2;
    xIpPlugConf.DPREGStart                 = 80;
    xIpPlugConf.DPREGEnd                   = 639;
    xIpPlugConf.WLRURatio                  = 15;
    ret = HAL_DCMIPP_SetIPPlugConfig( pxHdcmipp, &xIpPlugConf );
    assert( ret == HAL_OK );
}

static void prvDCMIPP_ReduceSpurious( DCMIPP_HandleTypeDef * pxHdcmipp )
{
    int ret;

    ret = HAL_DCMIPP_PIPE_EnableLineEvent( pxHdcmipp, DCMIPP_PIPE1, DCMIPP_MULTILINE_128_LINES );
    assert( ret == HAL_OK );
    ret = HAL_DCMIPP_PIPE_DisableLineEvent( pxHdcmipp, DCMIPP_PIPE1 );
    assert( ret == HAL_OK );
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void MediaCam_Init( void )
{
    CMW_Sensor_Name_t eSensor;
    CMW_CameraInit_t  xCamInit;
    int ret;

    /* --- Step 1 : detect sensor via CMW_CAMERA_GetSensorName() ---
     *
     * GetSensorName runs a Probe_Sensor chain with width=0/height=0/fps=30.
     * For IMX335 this still leaves the sensor in a known state (only one
     * resolution mode IMX335_R2592_1944 exists), but fps will be 30 rather
     * than our target CAMERA_FPS.  The subsequent CMW_CAMERA_Init below
     * re-runs Probe_Sensor with the correct fps.  Mirrors the reference
     * flow in x-cube-n6-ai-h264-usb-uvc/Src/app_cam.c:CAM_Init.            */
    vPetWatchdog();
    raw_puts( "[CAM] GSN>\r\n" );
    ret = CMW_CAMERA_GetSensorName( &eSensor );
    raw_puts( "[CAM] GSN<\r\n" );

    /* Check if GetSensorName left interrupts globally disabled */
    {
        uint32_t ulPrimask;
        __asm volatile( "mrs %0, primask" : "=r"( ulPrimask ) );
        raw_puts( "[CAM] PM=" );
        raw_putc( '0' + ( char )( ulPrimask & 0xF ) );
        raw_puts( "\r\n" );
        if( ulPrimask & 1 )
        {
            raw_puts( "[CAM] IRQs OFF! re-enabling\r\n" );
            __asm volatile( "cpsie i" ::: "memory" );
        }
    }

    vPetWatchdog();
    raw_puts( "[CAM] ret=" );
    raw_putc( '0' + ( char )( ret & 0xF ) );
    raw_puts( " sen=" );
    raw_putc( '0' + ( char )( eSensor & 0xF ) );
    raw_puts( "\r\n" );
    if( ret != CMW_ERROR_NONE )
    {
        raw_puts( "[CAM] FAIL\r\n" );
        return;
    }

    raw_puts( "[CAM] S2 setSensorInfo\r\n" );
    vPetWatchdog();
    prvSetSensorInfo( eSensor );

    /* --- Step 2 : full sensor re-init at target resolution / fps ---
     *
     * CMW_CAMERA_Init re-runs Probe_Sensor.  For IMX335, CMW_CAMERA_IMX335_Init
     * memsets camera_bsp and calls CMW_IMX335_Probe + IMX335_Init again with
     * our requested width/height/fps/mirror_flip values.  The driver programs
     * IMX335_R2592_1944 mode at CAMERA_FPS.                                   */
    xCamInit.width       = xSensorWidth;
    xCamInit.height      = xSensorHeight;
    xCamInit.fps         = CAMERA_FPS;
    xCamInit.mirror_flip = xSensorMirrorFlip;

    raw_puts( "[CAM] S3 Init>\r\n" );
    vPetWatchdog();
    ret = CMW_CAMERA_Init( &xCamInit, NULL );
    vPetWatchdog();
    raw_puts( "[CAM] S3 Init<r=" );
    raw_putc( '0' + ( char )( ret & 0xF ) );
    raw_puts( "\r\n" );
    if( ret != CMW_ERROR_NONE )
    {
        raw_puts( "[CAM] S3 FAIL\r\n" );
        return;
    }

    /* Write back the sensor dims that may have been adjusted by the driver */
    xSensorWidth  = xCamInit.width;
    xSensorHeight = xCamInit.height;

    raw_puts( "[CAM] S5 ipplug\r\n" );
    vPetWatchdog();
    prvDCMIPP_IpPlugInit( CMW_CAMERA_GetDCMIPPHandle() );

    raw_puts( "[CAM] S5 pipeEnc\r\n" );
    vPetWatchdog();
    prvDCMIPP_PipeInitEncoder( xSensorWidth, xSensorHeight );

    raw_puts( "[CAM] S5 spurious\r\n" );
    vPetWatchdog();
    prvDCMIPP_ReduceSpurious( CMW_CAMERA_GetDCMIPPHandle() );
    vPetWatchdog();

    raw_puts( "[CAM] S5 IRQ gate open\r\n" );
    {
        extern volatile uint8_t g_dcmipp_irq_ready;
        g_dcmipp_irq_ready = 1;
    }
    HAL_NVIC_EnableIRQ( CSI_IRQn );
    HAL_NVIC_EnableIRQ( DCMIPP_IRQn );

    raw_puts( "[CAM] DONE\r\n" );
}

void MediaCam_Start( uint8_t * pFrameBuffer, uint32_t ulCamMode )
{
    int ret = CMW_CAMERA_Start( DCMIPP_PIPE1, pFrameBuffer, ulCamMode );
    assert( ret == CMW_ERROR_NONE );
}

void MediaCam_StartDouble( uint8_t * pY0, uint8_t * pUV0,
                           uint8_t * pY1, uint8_t * pUV1,
                           uint32_t  ulCamMode )
{
    DCMIPP_HandleTypeDef * pxHdcmipp = CMW_CAMERA_GetDCMIPPHandle();
    DCMIPP_SemiPlanarDstAddressTypeDef xAddr0;
    DCMIPP_SemiPlanarDstAddressTypeDef xAddr1;
    int ret;

    assert( pY0 != NULL && pUV0 != NULL && pY1 != NULL && pUV1 != NULL );

    /* Lazily create the binary semaphore so that FrameEventCallback (which
     * may fire immediately after the first VSYNC) always finds a valid
     * handle.  Start with it empty — first frame must cause a "give".       */
    if( xFrameReadySem == NULL )
    {
        xFrameReadySem = xSemaphoreCreateBinary();
        configASSERT( xFrameReadySem != NULL );
    }

    pucPingPongY[ 0 ]  = pY0;
    pucPingPongUV[ 0 ] = pUV0;
    pucPingPongY[ 1 ]  = pY1;
    pucPingPongUV[ 1 ] = pUV1;
    ulFrameEventCount  = 0;

    /* Configure RGB→YUV color conversion on PIPE1 so the pixel packer can
     * emit YUV420_2 (NV12).  The CMW middleware only wires this up for the
     * YUV422_1 format; for the semi-planar format we must program the
     * matrix ourselves.  Coefficients are BT.601 full-range RGB→YCbCr, the
     * same ones the middleware uses internally for YUV422_1 in
     * cmw_camera.c:2105.                                                    */
    {
        #define N10( v )  ( ( ( v ) ^ 0x7FF ) + 1 )
        DCMIPP_ColorConversionConfTypeDef xYuvConv = {
            .ClampOutputSamples = ENABLE,
            .OutputSamplesType  = 0,
            .RR = 131,      .RG = N10( 110 ), .RB = N10( 21 ), .RA = 128,
            .GR = 77,       .GG = 150,        .GB = 29,        .GA = 0,
            .BR = N10( 44 ), .BG = N10( 87 ), .BB = 131,       .BA = 128,
        };
        #undef N10

        ret = HAL_DCMIPP_PIPE_SetYUVConversionConfig( pxHdcmipp, DCMIPP_PIPE1, &xYuvConv );
        assert( ret == HAL_OK );
        ret = HAL_DCMIPP_PIPE_EnableYUVConversion( pxHdcmipp, DCMIPP_PIPE1 );
        assert( ret == HAL_OK );
    }

    /* Print HAL state before DBM start so we can see whether the pipe is
     * READY (the HAL precondition) or stuck in BUSY/ERROR.                  */
    {
        DCMIPP_HandleTypeDef * pxH = CMW_CAMERA_GetDCMIPPHandle();
        raw_puts( "[CAM] pre-DBL State=" ); raw_hex32( ( uint32_t ) pxH->State );
        raw_puts( " P1State=" ); raw_hex32( ( uint32_t ) pxH->PipeState[ DCMIPP_PIPE1 ] );
        raw_puts( "\r\n" );
    }

    /* Diagnostic: confirm Camera_Drv.Start(&camera_bsp) will actually be
     * invoked by CMW_CAMERA_DoubleBufferStart.  That middleware function
     * guards the sensor-start call with `if (!is_camera_started)`, so if
     * the flag is already non-zero the IMX335 MODE_STREAMING I2C write
     * (and the ISP_Start call) are both skipped, which would explain why
     * the CSI receiver sees no SOF/EOF packets despite DBL start returning
     * success.                                                              */
    {
        extern int is_camera_started;
        raw_puts( "[CAM] is_cam_started=" );
        raw_dec( ( uint32_t ) is_camera_started );
        raw_puts( "\r\n" );
    }

    /* Assemble the semi-planar DBM address pairs.  DCMIPP checks 16-byte
     * alignment of both Y and UV addresses in the HAL start routine.        */
    xAddr0.YAddress  = ( uint32_t ) pY0;
    xAddr0.UVAddress = ( uint32_t ) pUV0;
    xAddr1.YAddress  = ( uint32_t ) pY1;
    xAddr1.UVAddress = ( uint32_t ) pUV1;

    /* Bypass CMW_CAMERA_DoubleBufferStart — that middleware helper only
     * plumbs the single-plane DBM API (HAL_DCMIPP_CSI_PIPE_DoubleBufferStart)
     * and doesn't know about the UV plane destination registers.  Call the
     * semi-planar HAL directly; then trigger Camera_Drv.Start() by borrowing
     * the "is_camera_started" guard the middleware uses for CMW_CAMERA_Start /
     * CMW_CAMERA_DoubleBufferStart.                                          */
    raw_puts( "[CAM] DBL SP start>\r\n" );
    ret = HAL_DCMIPP_CSI_PIPE_SemiPlanarDoubleBufferStart(
              pxHdcmipp, DCMIPP_PIPE1, DCMIPP_VIRTUAL_CHANNEL0,
              &xAddr0, &xAddr1, ulCamMode );
    raw_puts( "[CAM] DBL SP start<r=" ); raw_dec( ( uint32_t ) ret ); raw_puts( "\r\n" );
    assert( ret == HAL_OK );

    /* The semi-planar HAL start enables the DCMIPP capture but does NOT
     * start the sensor.  CMW_CAMERA_DoubleBufferStart normally calls
     * Camera_Drv.Start(&camera_bsp) for that, but Camera_Drv and camera_bsp
     * are static to cmw_camera.c so we can't call them directly.
     *
     * Previously this block poked the IMX335 MODE_SELECT register directly
     * via BSP I2C — that kicked the sensor into streaming, but it skipped
     * ISP_Init() which runs inside CMW_IMX335_Start before the I2C write.
     * Without ISP_Init, the ISP handle stored in CMW_IMX335_t stays zeroed,
     * and the first call to MediaCam_IspUpdate -> ISP_BackgroundProcess
     * dereferences it and takes a NULL-pointer bus fault
     * (CFSR=0x00008200, BFAR=0x00000000, in ISP_Algo_Process).
     *
     * Use the new CMW_CAMERA_StartSensorOnly helper which runs the full
     * ISP_Init + ISP_Start + IMX335_Start (=MODE_STREAMING I2C write)
     * sequence via the existing sensor driver — this leaves hIsp valid
     * so the background ISP algorithm loop can run.                         */
    {
        int32_t lS = CMW_CAMERA_StartSensorOnly();
        raw_puts( "[CAM] sensor start r=" ); raw_dec( ( uint32_t ) lS );
        raw_puts( "\r\n" );
        assert( lS == 0 );
    }

    /* And after — should still be 1 (Camera_Drv.Start either ran just now
     * or had already run previously).                                       */
    {
        extern int is_camera_started;
        raw_puts( "[CAM] post is_cam_started=" );
        raw_dec( ( uint32_t ) is_camera_started );
        raw_puts( "\r\n" );
    }

    /* Direct I2C probe of the IMX335 sensor to verify:
     *   (a) sensor is still alive on the I2C bus (read chip ID)
     *   (b) MODE_SELECT register is 0x00 = STREAMING (set by IMX335_Start)
     *
     * If ID read fails, the sensor is dead/unresponsive.
     * If MODE_SELECT reads 0x01 = STANDBY, the I2C write to start streaming
     * that CMW_CAMERA_DoubleBufferStart performed did not actually land —
     * somehow the sensor is still in standby even though we thought we'd
     * started it.  This is the key discriminator for why CSI_SR0 shows no
     * SOF/EOF activity.                                                    */
    {
        uint8_t  ucId = 0xFF;
        uint8_t  ucMode = 0xFF;
        int32_t  lR1, lR2;

        /* 0x34 = 8-bit I2C address form for the IMX335, matches
         * CAMERA_IMX335_ADDRESS in cmw_io.h                                 */
        lR1 = BSP_I2C1_ReadReg16( 0x34, 0x3912 /* IMX335_REG_ID */,      &ucId,   1 );
        lR2 = BSP_I2C1_ReadReg16( 0x34, 0x3000 /* IMX335_REG_MODE_SEL */, &ucMode, 1 );

        raw_puts( "[CAM] sensor I2C: ID_r=" ); raw_dec( ( uint32_t )( lR1 & 0xFF ) );
        raw_puts( " ID="  ); raw_hex32( ucId );
        raw_puts( " MODE_r=" ); raw_dec( ( uint32_t )( lR2 & 0xFF ) );
        raw_puts( " MODE=" ); raw_hex32( ucMode );
        raw_puts( "\r\n" );
    }

    /* CRITICAL: clear the sticky CSI flag registers BEFORE dumping so that
     * the subsequent WaitFrame timeout dumps show only NEW events that
     * accumulated during the wait, not the leftover ULPN transition events
     * that were latched way back during PHY init.
     *
     * Previous run observed SR1=0x64000000 (ULPNDL1F|ULPNACTF|ULPNCLF) and
     * the values never changed across multiple 2-second timeout dumps —
     * that could mean either (a) the lanes are completely dead and the
     * bits are stuck at their post-init latched values, or (b) there IS
     * activity but we can't see it because the flags are already set.
     * Clearing forces interpretation (a) vs (b) to be distinguishable:
     *   - If SR1 stays 0 across timeouts → lanes are DEAD (no D-PHY signal)
     *   - If SR1 accumulates new STOP/ACT/ULPN → sensor IS driving lanes
     *     but something else (e.g. DTID mismatch) blocks the VC0 capture. */
    CSI->FCR0 = 0xFFFFFFFFUL;
    CSI->FCR1 = 0xFFFFFFFFUL;

    /* Dump DCMIPP + CSI state immediately after start.  Any difference in
     * subsequent WaitFrame dumps indicates a runtime event.
     *
     * CSI PHY register sanity targets for IMX335 @ 1600 Mbps/lane, 2 lanes:
     *   PFCR   = 0x00010D28  (CCFR=0x28, HSFR=0x0D, DLD=1)
     *   LMCFGR = 0x00210200  (LANENB=2@b8, DL0MAP=1@b16, DL1MAP=2@b20)
     *   PRCR   = 0x00000002  (PEN=1 → PHY out of reset)
     *   PMCR   = 0x00000000  (no force modes)
     *   PCR    = 0x0000000F  (DL0EN|DL1EN|CLEN|PWRDOWN, all enabled)
     *   PTSR   = PHY test status — bit layout TRM, non-zero means lock/OK
     * If any of these read back different, HAL_DCMIPP_CSI_SetConfig never
     * landed the value (or something clobbered it in the idle window).    */
    {
        DCMIPP_TypeDef * pxDc = CMW_CAMERA_GetDCMIPPHandle()->Instance;
        raw_puts( "[CAM] post-DBL CMSR2=" ); raw_hex32( pxDc->CMSR2 );
        raw_puts( " CMIER=" ); raw_hex32( pxDc->CMIER );
        raw_puts( "\r\n  P1FSCR=" ); raw_hex32( pxDc->P1FSCR );
        raw_puts( " P1FCTCR=" ); raw_hex32( pxDc->P1FCTCR );
        raw_puts( "\r\n  P1PPCR=" ); raw_hex32( pxDc->P1PPCR );
        raw_puts( " CMCR=" ); raw_hex32( pxDc->CMCR );
        raw_puts( "\r\n  CSI_CR=" ); raw_hex32( CSI->CR );
        raw_puts( " CSI_PCR=" ); raw_hex32( CSI->PCR );
        raw_puts( "\r\n  CSI_SR0=" ); raw_hex32( CSI->SR0 );
        raw_puts( " CSI_SR1=" ); raw_hex32( CSI->SR1 );
        raw_puts( "\r\n  CSI_ERR1=" ); raw_hex32( CSI->ERR1 );
        raw_puts( " ERR2=" ); raw_hex32( CSI->ERR2 );
        raw_puts( "\r\n  CSI_VC0CFGR1=" ); raw_hex32( CSI->VC0CFGR1 );
        raw_puts( " VC0CFGR2=" ); raw_hex32( CSI->VC0CFGR2 );
        raw_puts( "\r\n  CSI_PRCR=" ); raw_hex32( CSI->PRCR );
        raw_puts( " LMCFGR=" ); raw_hex32( CSI->LMCFGR );
        raw_puts( "\r\n  CSI_PFCR=" ); raw_hex32( CSI->PFCR );
        raw_puts( " PMCR=" ); raw_hex32( CSI->PMCR );
        raw_puts( "\r\n  CSI_PTSR=" ); raw_hex32( CSI->PTSR );
        raw_puts( "\r\n" );
    }

    /* ── FCR1 write-clear verification ───────────────────────────────────────
     *
     * Read SR1, write 0xFFFFFFFF to FCR1, read SR1 again.  If FCR1 is a
     * functional w1c register for SR1's latched bits, `after` should be 0
     * (or at least differ from `before`).  If `after == before`, SR1 bits
     * are either live state (not clearable via FCR1) or FCR1 targets a
     * different set of flags (e.g. interrupt/DMA control, not status).
     * This directly resolves the previous ambiguity where SR1=0x64000000
     * appeared identical in post-DBL and timeout dumps despite FCR clears. */
    {
        uint32_t ulBefore;
        uint32_t ulAfter;

        ulBefore = CSI->SR1;
        CSI->FCR1 = 0xFFFFFFFFUL;
        __DSB();
        ulAfter  = CSI->SR1;

        raw_puts( "  FCR1 clr test SR1 before=" ); raw_hex32( ulBefore );
        raw_puts( " after=" ); raw_hex32( ulAfter ); raw_puts( "\r\n" );
    }

    /* ── Extended CSI activity poll ──────────────────────────────────────────
     *
     * Watch SR0/SR1 for 3 seconds after DBL start, dumping every change.
     * Normal behavior when a sensor is streaming at 15 fps is a 66 ms
     * cadence of state transitions as each frame burst starts/ends on the
     * data lanes (SR0 SOF0F/EOF0F pulses, SR1 STOP/ACT bit wiggles).
     *
     * If we see NOTHING over 3 seconds, the IMX335 MIPI TX is not driving
     * the lanes at all — despite the sensor reporting MODE_SELECT=0x00.
     * If we see activity but no frame event, the issue is downstream of
     * the CSI PHY (DCMIPP pipe config, DBM address latch, etc.).
     * Polls every 10 ms via vTaskDelay so FreeRTOS timers / TCP keepalive
     * still run; pets the watchdog each iteration.                         */
    {
        uint32_t ulSr0Prev;
        uint32_t ulSr1Prev;
        int      iIter;

        ulSr0Prev = CSI->SR0;
        ulSr1Prev = CSI->SR1;
        raw_puts( "  poll t0 SR0=" ); raw_hex32( ulSr0Prev );
        raw_puts( " SR1=" ); raw_hex32( ulSr1Prev ); raw_puts( "\r\n" );

        for( iIter = 0; iIter < 300; iIter++ )
        {
            uint32_t ulSr0Cur;
            uint32_t ulSr1Cur;

            vTaskDelay( pdMS_TO_TICKS( 10 ) );
            vPetWatchdog();

            ulSr0Cur = CSI->SR0;
            ulSr1Cur = CSI->SR1;

            if( ( ulSr0Cur != ulSr0Prev ) || ( ulSr1Cur != ulSr1Prev ) )
            {
                raw_puts( "  chg t=" ); raw_dec( ( uint32_t ) iIter );
                raw_puts( "0ms SR0=" ); raw_hex32( ulSr0Cur );
                raw_puts( " SR1=" ); raw_hex32( ulSr1Cur );
                raw_puts( "\r\n" );
                ulSr0Prev = ulSr0Cur;
                ulSr1Prev = ulSr1Cur;
            }

            if( ulFrameEventCount > 0U )
            {
                raw_puts( "  FRAME at t=" ); raw_dec( ( uint32_t ) iIter );
                raw_puts( "0ms\r\n" );
                break;
            }
        }

        raw_puts( "  poll end iter=" ); raw_dec( ( uint32_t ) iIter );
        raw_puts( " SR0=" ); raw_hex32( CSI->SR0 );
        raw_puts( " SR1=" ); raw_hex32( CSI->SR1 );
        raw_puts( " fec=" ); raw_dec( ulFrameEventCount );
        raw_puts( "\r\n" );
    }
}

void MediaCam_Stop( void )
{
    DCMIPP_HandleTypeDef * pxH = CMW_CAMERA_GetDCMIPPHandle();
    HAL_StatusTypeDef      rc;

    raw_puts( "[CAM] Stop>\r\n" );

    /* If the pipe is in ERROR state (e.g. from an OVR that permanently
     * wedged it), force it to BUSY so HAL_DCMIPP_CSI_PIPE_Stop can
     * process it — that function rejects RESET but accepts everything
     * else and transitions to READY.                                       */
    if( pxH->PipeState[ DCMIPP_PIPE1 ] == HAL_DCMIPP_PIPE_STATE_ERROR )
    {
        pxH->PipeState[ DCMIPP_PIPE1 ] = HAL_DCMIPP_PIPE_STATE_BUSY;
    }

    /* Stop the DCMIPP pipe 1 capture + CSI virtual-channel 0.
     * This clears CPTREQ, waits for P1CPTACT to deassert (~1s timeout),
     * disables DBM + pipe + pipe interrupts, stops the CSI VC, and
     * transitions PipeState to READY.                                      */
    rc = HAL_DCMIPP_CSI_PIPE_Stop( pxH, DCMIPP_PIPE1,
                                   DCMIPP_VIRTUAL_CHANNEL0 );
    raw_puts( "[CAM] pipe stop r=" ); raw_dec( ( uint32_t ) rc );
    raw_puts( "\r\n" );

    if( rc != HAL_OK )
    {
        /* Graceful stop failed (hardware wedged).  Force PipeState to
         * READY so the next SemiPlanarDoubleBufferStart doesn't reject
         * on state check.  The hardware may need a full DCMIPP reset in
         * this path — for now, force-clearing state is the best effort.    */
        raw_puts( "[CAM] WARN force PipeState→READY\r\n" );
        pxH->PipeState[ DCMIPP_PIPE1 ] = HAL_DCMIPP_PIPE_STATE_READY;
    }

    /* Put the IMX335 sensor into standby (MODE_SELECT = 0x01) so the
     * MIPI TX lanes go idle.  On restart, CMW_CAMERA_StartSensorOnly()
     * will re-run ISP_Init + ISP_Start + IMX335_Start (MODE_STREAMING).    */
    {
        uint8_t ucStandby = 0x01;
        BSP_I2C1_WriteReg16( 0x34, 0x3000 /* IMX335_REG_MODE_SEL */,
                             &ucStandby, 1 );
        raw_puts( "[CAM] sensor→standby\r\n" );
    }

    /* Decrement is_camera_started so the next StartSensorOnly() call
     * re-runs the full ISP_Init + sensor-start sequence.                   */
    {
        extern int is_camera_started;
        if( is_camera_started > 0 )
        {
            is_camera_started--;
        }
        raw_puts( "[CAM] is_cam_started=" );
        raw_dec( ( uint32_t ) is_camera_started );
        raw_puts( "\r\n" );
    }

    /* Reset frame-event state for the next session */
    ulFrameEventCount = 0;

    raw_puts( "[CAM] Stop<\r\n" );
}

MediaCamFrame_t MediaCam_WaitFrame( void )
{
    extern volatile uint32_t g_dcmipp_irq_count;
    MediaCamFrame_t xRes;
    uint32_t ulCount;
    uint32_t ulIdx;

    assert( xFrameReadySem != NULL );

    /* Wait with a 2s timeout so we can dump diagnostics if the DCMIPP
     * frame-event ISR isn't firing.  On timeout, print DCMIPP register
     * state (CMSR2/CMIER/P1FCTCR/P1PPCR/P1PPM0AR1/P1PPM0AR2) and retry.    */
    while( 1 )
    {
        if( xSemaphoreTake( xFrameReadySem, pdMS_TO_TICKS( 2000U ) ) == pdTRUE )
        {
            break;
        }

        /* TIMEOUT — dump diagnostics and keep waiting */
        {
            DCMIPP_TypeDef * pxDcmipp = CMW_CAMERA_GetDCMIPPHandle()->Instance;
            raw_puts( "[CAM] TIMEOUT irq_cnt=" );
            raw_dec( g_dcmipp_irq_count );
            raw_puts( " fec=" );
            raw_dec( ulFrameEventCount );
            raw_puts( "\r\n" );
            raw_puts( "  CMSR2="     ); raw_hex32( pxDcmipp->CMSR2    );
            raw_puts( " CMIER="      ); raw_hex32( pxDcmipp->CMIER    );
            raw_puts( "\r\n  P1FCTCR=" ); raw_hex32( pxDcmipp->P1FCTCR );
            raw_puts( " P1FSCR="     ); raw_hex32( pxDcmipp->P1FSCR   );
            raw_puts( "\r\n  P1PPCR=" ); raw_hex32( pxDcmipp->P1PPCR  );
            raw_puts( " AR1="        ); raw_hex32( pxDcmipp->P1PPM0AR1 );
            raw_puts( " AR2="        ); raw_hex32( pxDcmipp->P1PPM0AR2 );
            raw_puts( "\r\n  CMCR="  ); raw_hex32( pxDcmipp->CMCR     );
            raw_puts( " CMSR1="      ); raw_hex32( pxDcmipp->CMSR1    );
            raw_puts( "\r\n" );
            /* Also probe the NVIC to confirm DCMIPP_IRQn is enabled */
            {
                uint32_t ulIser = NVIC->ISER[ ( uint32_t )( DCMIPP_IRQn ) >> 5U ];
                uint32_t ulPend = NVIC->ISPR[ ( uint32_t )( DCMIPP_IRQn ) >> 5U ];
                raw_puts( "  NVIC_ISER=" ); raw_hex32( ulIser );
                raw_puts( " NVIC_ISPR="  ); raw_hex32( ulPend );
                raw_puts( "\r\n" );
            }
            /* Dump CSI receiver status — tells us whether the sensor is
             * actually streaming or whether the CSI PHY is silent.          */
            {
                raw_puts( "  CSI_CR="    ); raw_hex32( CSI->CR    );
                raw_puts( " PCR="       ); raw_hex32( CSI->PCR   );
                raw_puts( "\r\n  CSI_SR0=" ); raw_hex32( CSI->SR0   );
                raw_puts( " SR1="       ); raw_hex32( CSI->SR1   );
                raw_puts( "\r\n  CSI_ERR1=" ); raw_hex32( CSI->ERR1 );
                raw_puts( " ERR2="      ); raw_hex32( CSI->ERR2  );
                raw_puts( "\r\n  CSI_PRCR=" ); raw_hex32( CSI->PRCR );
                raw_puts( " LMCFGR="    ); raw_hex32( CSI->LMCFGR );
                raw_puts( "\r\n  CSI_PFCR=" ); raw_hex32( CSI->PFCR );
                raw_puts( " PMCR="      ); raw_hex32( CSI->PMCR );
                raw_puts( "\r\n  CSI_PTSR=" ); raw_hex32( CSI->PTSR );
                raw_puts( "\r\n" );
            }
            /* And the HAL DCMIPP driver's internal state machine */
            {
                DCMIPP_HandleTypeDef * pxH = CMW_CAMERA_GetDCMIPPHandle();
                raw_puts( "  HAL_State=" ); raw_hex32( ( uint32_t ) pxH->State );
                raw_puts( " P1State=" ); raw_hex32( ( uint32_t ) pxH->PipeState[ DCMIPP_PIPE1 ] );
                raw_puts( "\r\n" );
            }
        }
    }

    /* Snapshot ulFrameEventCount with interrupts masked so the ISR can't
     * increment it mid-read.  (It's a 32-bit volatile on an M55 so the
     * read is atomic anyway, but we want a coherent value for the index.)  */
    ulCount = ulFrameEventCount;

    /* ulCount=1 → slot 0 just done; ulCount=2 → slot 1 just done; etc.      */
    ulIdx   = ( ulCount - 1U ) & 1U;
    xRes.pY  = pucPingPongY[ ulIdx ];
    xRes.pUV = pucPingPongUV[ ulIdx ];
    return xRes;
}

void MediaCam_IspUpdate( void )
{
    int ret = CMW_CAMERA_Run();
    assert( ret == CMW_ERROR_NONE );
}

int MediaCam_GetWidth( void )
{
    assert( xVencWidth );
    return xVencWidth;
}

int MediaCam_GetHeight( void )
{
    assert( xVencHeight );
    return xVencHeight;
}

/* DCMIPP pipe error recovery.
 *
 * Called from the DCMIPP_IRQn ISR after the HAL OVR handler (see
 * stm32n6xx_hal_dcmipp.c:2307).  By the time we run, the HAL has already:
 *   - Disabled the PIPE1 OVR interrupt (__HAL_DCMIPP_DISABLE_IT)
 *   - Cleared the OVR flag in CMFCR
 *   - Set hdcmipp->ErrorCode |= HAL_DCMIPP_ERROR_PIPE1_OVR
 *   - Set hdcmipp->PipeState[1] = HAL_DCMIPP_PIPE_STATE_ERROR
 *
 * Crucially the pipe HARDWARE keeps running and FRAME interrupts keep
 * firing — only the OVR IT is masked.  The HAL's PipeState going to
 * ERROR would cause subsequent HAL_DCMIPP_* guarded calls to bail with
 * HAL_ERROR, so we reset it back to BUSY.
 *
 * IMPORTANT: We deliberately do NOT re-enable the OVR interrupt.  An
 * earlier version of this callback did, and under heavy PSRAM/AXI
 * contention the OVR flag re-asserted immediately every time, producing
 * a storm of ~100-200 ISR re-entries per frame period.  Each re-entry
 * printed a char over 115200 baud (~87us busy-wait in ISR context), so
 * a 200-char storm pinned the CPU in IRQ for ~17ms every 100ms frame
 * period.  The storm ultimately wedged the media pipeline on downstream
 * h264 send paths.
 *
 * Leaving the OVR IT disabled is safe: frame events continue, no frames
 * are lost beyond those already overrun (hardware issue, not
 * observability), and we simply lose the ability to detect subsequent
 * overruns — which we don't need for recovery.  The media task's 30-
 * frame heartbeat now reports the OVR counter so we still have
 * visibility without ISR-context UART traffic.                         */
#ifndef HAL_DCMIPP_ERROR_PIPE1_OVR
#define HAL_DCMIPP_ERROR_PIPE1_OVR  (0x00000040U)
#endif

volatile uint32_t g_dcmipp_ovr_count = 0;

void CMW_CAMERA_PIPE_ErrorCallback( uint32_t ulPipe )
{
    DCMIPP_HandleTypeDef * pxH;

    pxH = CMW_CAMERA_GetDCMIPPHandle();
    if( pxH == NULL )
    {
        return;
    }

    if( ulPipe == DCMIPP_PIPE1 )
    {
        /* Bump the counter (read by the media task for periodic reporting) */
        g_dcmipp_ovr_count++;

        /* Clear the HAL ErrorCode OVR bit so subsequent HAL calls don't
         * see a stale error.                                            */
        pxH->ErrorCode &= ~HAL_DCMIPP_ERROR_PIPE1_OVR;

        /* Return pipe state from ERROR back to BUSY — capture is still
         * running in the hardware; PipeState should reflect that.       */
        if( pxH->PipeState[ DCMIPP_PIPE1 ] == HAL_DCMIPP_PIPE_STATE_ERROR )
        {
            pxH->PipeState[ DCMIPP_PIPE1 ] = HAL_DCMIPP_PIPE_STATE_BUSY;
        }

        /* Do NOT touch CMFCR (HAL already cleared the flag).
         * Do NOT re-enable CMIER.P1OVRIE — see comment above.          */
    }
    else
    {
        /* PIPE2 path unused in this project.  Increment counter but don't
         * call logging from ISR.                                        */
        g_dcmipp_ovr_count++;
    }
}

/* ── MX_DCMIPP_ClockConfig — strong override ────────────────────────────────
 *
 * The Camera Middleware defines this as a weak no-op (cmw_camera.c:524) and
 * calls it from CMW_CAMERA_Init.  The application MUST override it to
 * configure the DCMIPP + CSI kernel clocks, otherwise the CSI D-PHY has no
 * "cfg_clk" reference and cannot run its escape-mode state machine:
 *   - SR1 lane flags freeze at their post-reset values (observed: 0x64000000
 *     with DL0 "in ULPS" forever)
 *   - No STOP/ACT transitions are ever detected on the data lanes
 *   - No SOF/EOF events arrive → no frame-event callback → media task hangs
 *
 * Clock budget on this project (set up by FSBL/Core/Src/main.c):
 *   HSI  = 64 MHz
 *   PLL1 = HSI * 25 / 1 / 1 / 1 = 1600 MHz (ON)
 *   PLL2 = OFF
 *   PLL3 = OFF
 *   PLL4 = HSI * 25 / 1 / 1 / 1 = 1600 MHz (ON)
 *
 * Target frequencies (matching the x-cube-n6-ai-h264-usb-uvc reference):
 *   IC17 → DCMIPP kernel = 320 MHz  (reference uses PLL2/3 ≈ 333 MHz;
 *                                     closest we can reach is PLL1/5 = 320)
 *   IC18 → CSI D-PHY cfg = 20 MHz   (reference uses PLL1/40 where PLL1=800
 *                                     MHz; our PLL1 is 1600 MHz so PLL1/80
 *                                     lands on the same 20 MHz target that
 *                                     the hardcoded CCFR=0x28 in
 *                                     HAL_DCMIPP_CSI_SetConfig assumes)      */
HAL_StatusTypeDef MX_DCMIPP_ClockConfig( DCMIPP_HandleTypeDef * pxHdcmipp )
{
    RCC_PeriphCLKInitTypeDef xPeriphClk = { 0 };
    HAL_StatusTypeDef         xRet;

    ( void ) pxHdcmipp;

    /* CSI cfg-clock (IC18) = PLL1 / 80 = 20 MHz */
    xPeriphClk.PeriphClockSelection               = RCC_PERIPHCLK_CSI;
    xPeriphClk.ICSelection[ RCC_IC18 ].ClockSelection = RCC_ICCLKSOURCE_PLL1;
    xPeriphClk.ICSelection[ RCC_IC18 ].ClockDivider   = 80;
    xRet = HAL_RCCEx_PeriphCLKConfig( &xPeriphClk );
    if( xRet != HAL_OK )
    {
        return xRet;
    }

    /* DCMIPP kernel clock (IC17) = PLL1 / 5 = 320 MHz */
    xPeriphClk.PeriphClockSelection                 = RCC_PERIPHCLK_DCMIPP;
    xPeriphClk.DcmippClockSelection                 = RCC_DCMIPPCLKSOURCE_IC17;
    xPeriphClk.ICSelection[ RCC_IC17 ].ClockSelection = RCC_ICCLKSOURCE_PLL1;
    xPeriphClk.ICSelection[ RCC_IC17 ].ClockDivider   = 5;
    xRet = HAL_RCCEx_PeriphCLKConfig( &xPeriphClk );
    if( xRet != HAL_OK )
    {
        return xRet;
    }

    raw_puts( "[CAM] IC17=PLL1/5(320M) IC18=PLL1/80(20M)\r\n" );
    return HAL_OK;
}

/* Weak-override of CMW middleware's frame-event dispatch.  Fires from the
 * DCMIPP_IRQn ISR at the end of every captured frame.  In double-buffer
 * mode we use it to wake MediaCam_WaitFrame().
 *
 * IMPORTANT: Do NOT emit UART characters from this ISR.  Per-frame raw_putc
 * here spun the CPU waiting for TXE on the 115200-baud UART (~87us per
 * char) inside an interrupt handler, back-pressuring higher-priority IRQs
 * and worsening AXI contention that likely precipitated DCMIPP OVR events. */
int CMW_CAMERA_PIPE_FrameEventCallback( uint32_t ulPipe )
{
    if( ( ulPipe == DCMIPP_PIPE1 ) && ( xFrameReadySem != NULL ) )
    {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;

        ulFrameEventCount++;

        /* Giving a binary sem that is already "available" is a no-op and
         * does not stack — if the media task is slow and we miss events,
         * the task will still wake and use the most recent completed index
         * from ulFrameEventCount.                                           */
        ( void ) xSemaphoreGiveFromISR( xFrameReadySem, &xHigherPriorityTaskWoken );
        portYIELD_FROM_ISR( xHigherPriorityTaskWoken );
    }
    return 0;
}
