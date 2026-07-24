/*
 * ai_detection.c — NPU object detection alongside KVS WebRTC streaming.
 *
 * Adapted from x-cube-n6-ai-h264-usb-uvc (app.c / app_cam.c / main.c /
 * utils.c) for this firmware's media pipeline: DCMIPP PIPE1 keeps feeding
 * NV12 to VENC untouched; PIPE2 downscales the same sensor stream to the
 * model input (RGB888, model-native WxH) into a double buffer consumed by
 * a low-priority inference task.  Detections are logged (Phase 3) and
 * published as IOTCONNECT telemetry (Phase 4).
 *
 * Compiled out entirely unless ENABLE_AI_DETECTION is defined — the
 * streaming firmware must build and behave identically without it.
 */

#if defined( ENABLE_AI_DETECTION )

#include <assert.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "stm32n6xx_hal.h"
#include "cmw_camera.h"

#include "logging_levels.h"
#define LOG_LEVEL    LOG_INFO
#include "logging.h"

#include "stai_network.h"
#include "npu_cache.h"
#include "app_postprocess.h"
#include "od_yolov2_pp_if.h"
#include "lcd_preview.h"          /* LcdBox_t + overlay update */

#include "ai_detection.h"

/* ── Model geometry ─────────────────────────────────────────────────────── */
#define AI_NN_WIDTH    STAI_NETWORK_IN_1_WIDTH
#define AI_NN_HEIGHT   STAI_NETWORK_IN_1_HEIGHT
#define AI_NN_BPP      3
#define AI_NN_FORMAT   DCMIPP_PIXEL_PACKER_FORMAT_RGB888_YUV444_1

#define AI_OUT_BUFFER_MAX_BYTES   ( 64U * 1024U )

/* Floor between inference starts.  Every inference streams the weights over
 * XSPI2 and competes with DCMIPP/VENC/Wi-Fi for the NoC.  Rate-tuning
 * 2026-07-21: 500 ms (2 Hz) was the conservative bring-up floor; with
 * octal-DTR NOR at 200 MHz an inference costs 31-65 ms and the 5+ min
 * relay soak showed heap-flat 1 Mbps streaming alongside it, so 200 ms
 * (5 Hz, ~17% NPU duty) is the new floor — responsive enough for LCD
 * overlays.  Next step after the LCD lands and soaks: 100 ms. */
#define AI_MIN_INFER_INTERVAL_MS  ( 500U )   /* A/B: 200 (5Hz) wedge-test — back to validated 2Hz */

/* BISECT (2026-07-20) history: step 1 (PIPE2 off) -> sessions survive;
 * step 2 (PIPE2 on, inference skipped) -> video visible but PIPE1 frames
 * were changing garbage and the session died at ~10 s: the DCMIPP IPPLUG
 * partition in media_cam.c was starving PIPE1 the moment PIPE2 ran (see
 * prvDCMIPP_IpPlugInit).  With the repartition in place, inference is
 * back ON.  Set to 1 only for future A/B debugging. */
#define AI_BISECT_SKIP_INFERENCE  ( 0 )

#if defined ( __GNUC__ )
    #define ALIGN_32    __attribute__((aligned(32)))
    #define IN_PSRAM    __attribute__((section(".psram_bss")))
#endif

/* ── Buffers ────────────────────────────────────────────────────────────── */
static uint8_t ucNnInputBuf[ 2 ][ AI_NN_WIDTH * AI_NN_HEIGHT * AI_NN_BPP ] ALIGN_32 IN_PSRAM;
static uint8_t ucNnOutputBuf[ AI_OUT_BUFFER_MAX_BYTES ] ALIGN_32 IN_PSRAM;
static uint8_t network_ctx[ STAI_NETWORK_CONTEXT_SIZE ] ALIGN_32;

/* Telemetry snapshot (Phase 3) — updated by the inference task after each
 * postprocess, read lock-free by the IOTCONNECT app task (32-bit writes
 * are atomic on M55; values are independent best-effort stats). */
static volatile uint8_t  ucAiReady            = 0U;
static volatile int32_t  lLatestDetections    = 0;
static volatile uint32_t ulLatestTopConfPct   = 0U;
static volatile uint32_t ulLatestInferMs      = 0U;
static volatile uint32_t ulTotalInferences    = 0U;

uint8_t AiDetection_GetTelemetry( int32_t * plDetections,
                                  uint32_t * pulTopConfPct,
                                  uint32_t * pulInferMs,
                                  uint32_t * pulInferences )
{
    if( ucAiReady == 0U )
    {
        return 0U;
    }

    if( plDetections != NULL )   { *plDetections  = lLatestDetections; }
    if( pulTopConfPct != NULL )  { *pulTopConfPct = ulLatestTopConfPct; }
    if( pulInferMs != NULL )     { *pulInferMs    = ulLatestInferMs; }
    if( pulInferences != NULL )  { *pulInferences = ulTotalInferences; }

    return 1U;
}

/* Latest detection boxes (normalized) — multi-word record, so unlike the
 * scalar telemetry above both sides copy under a critical section. */
static LcdBox_t xBoxSnap[ LCD_PREVIEW_MAX_BOXES ];
static uint32_t ulBoxSnapCount = 0U;

uint32_t AiDetection_GetBoxes( LcdBox_t * pxBoxes, uint32_t ulMax )
{
    uint32_t ulCount;

    if( ( ucAiReady == 0U ) || ( pxBoxes == NULL ) || ( ulMax == 0U ) )
    {
        return 0U;
    }

    taskENTER_CRITICAL();
    ulCount = ( ulBoxSnapCount < ulMax ) ? ulBoxSnapCount : ulMax;
    ( void ) memcpy( pxBoxes, xBoxSnap, ulCount * sizeof( LcdBox_t ) );
    taskEXIT_CRITICAL();

    return ulCount;
}

/* Double-buffer state: DCMIPP writes ucNnInputBuf[ucCaptureIdx]; on frame
 * done the ISR flips the index, points PIPE2 at the fresh buffer and gives
 * the semaphore so the task processes the completed one. */
static volatile uint8_t  ucCaptureIdx  = 0U;
static volatile uint8_t  ucReadyIdx    = 0U;
static SemaphoreHandle_t xFrameReady   = NULL;
static volatile uint8_t  ucPipeRunning = 0U;

/* DIAG 2026-07-23: PIPE2 delivery counters, updated in the frame ISR.
 * ulP2Frames proves frame-done fires; ulP2CurAR captures the live shadow
 * packer address so we can see the buf0<->buf1 flip actually latch. */
static volatile uint32_t ulP2Frames = 0U;
static volatile uint32_t ulP2CurAR  = 0U;

/* ── PIPE1-frame -> NN input path ───────────────────────────────────────────
 * The DCMIPP PIPE2 shared fork is dead on this silicon (proven on-board: it
 * frames + writes every buffer but delivers only 0xFF, independent of every
 * PIPE1/PIPE2 register setting).  Instead the NPU is fed from the WORKING
 * PIPE1 frame: the media task hands us its just-completed NV12 640x480 frame
 * and we CPU-downscale + YUV->RGB it into the model input.
 *
 * Byte order: the donor's PIPE2 packer used enable_swap=1 (R/B swapped), so
 * the model's channel-0 is BLUE.  Match that by writing B,G,R.  If the input
 * probe shows live values but detections never fire, flip this to 0. */
#define AI_NN_CHANNEL_SWAP   ( 1 )

/* Set true while the media task has a frame downscaled + queued and the NPU
 * has not finished with it; blocks the media task from overwriting the input
 * mid-inference and naturally paces submission to the inference rate. */
static volatile uint8_t    ucAiBusy    = 0U;
static volatile TickType_t xLastSubmit = 0;

static inline uint8_t prvClamp8( int32_t lV )
{
    if( lV < 0 )     { return 0U; }
    if( lV > 255 )   { return 255U; }
    return ( uint8_t ) lV;
}

/* Nearest-neighbour downscale of an NV12 frame (srcW x srcH: Y plane then
 * interleaved UV, 4:2:0) to AI_NN_WIDTH x AI_NN_HEIGHT RGB888, with BT.601
 * full-range YUV->RGB (Q16 fixed point, matching the DCMIPP CSC we used). */
static void prvDownscaleNV12toRGB( const uint8_t * pucY, const uint8_t * pucUV,
                                   uint32_t ulSrcW, uint32_t ulSrcH,
                                   uint8_t * pucDst )
{
    uint32_t ulDy;

    for( ulDy = 0U; ulDy < AI_NN_HEIGHT; ulDy++ )
    {
        uint32_t        ulSy    = ( ulDy * ulSrcH ) / AI_NN_HEIGHT;
        const uint8_t * pucYrow  = &pucY[ ulSy * ulSrcW ];
        const uint8_t * pucUVrow = &pucUV[ ( ulSy >> 1 ) * ulSrcW ];
        uint32_t        ulDx;

        for( ulDx = 0U; ulDx < AI_NN_WIDTH; ulDx++ )
        {
            uint32_t ulSx = ( ulDx * ulSrcW ) / AI_NN_WIDTH;
            int32_t  lY   = ( int32_t ) pucYrow[ ulSx ];
            int32_t  lU   = ( int32_t ) pucUVrow[ ( ulSx & ~1U ) ]      - 128;
            int32_t  lV   = ( int32_t ) pucUVrow[ ( ulSx & ~1U ) + 1U ] - 128;
            int32_t  lR   = lY + ( ( 91881  * lV ) >> 16 );              /* 1.402  */
            int32_t  lG   = lY - ( ( 22554  * lU + 46802 * lV ) >> 16 ); /* .344/.714 */
            int32_t  lB   = lY + ( ( 116130 * lU ) >> 16 );             /* 1.772  */

#if ( AI_NN_CHANNEL_SWAP == 1 )
            *pucDst++ = prvClamp8( lB );
            *pucDst++ = prvClamp8( lG );
            *pucDst++ = prvClamp8( lR );
#else
            *pucDst++ = prvClamp8( lR );
            *pucDst++ = prvClamp8( lG );
            *pucDst++ = prvClamp8( lB );
#endif
        }
    }
}

void AiDetection_SubmitNV12( const uint8_t * pucY, const uint8_t * pucUV,
                             uint32_t ulSrcWidth, uint32_t ulSrcHeight )
{
    TickType_t xNow;

    if( ( ucAiReady == 0U ) || ( xFrameReady == NULL ) )   { return; }
    if( ucAiBusy != 0U )                                   { return; }  /* NPU still on prev frame */

    xNow = xTaskGetTickCount();
    if( ( xLastSubmit != 0 ) &&
        ( ( xNow - xLastSubmit ) < pdMS_TO_TICKS( AI_MIN_INFER_INTERVAL_MS ) ) )
    {
        return;
    }
    xLastSubmit = xNow;

    /* Claim BEFORE writing so a re-entrant/next-frame submit can't overwrite
     * the buffer while the NPU reads it; AiTask clears ucAiBusy when done. */
    ucAiBusy   = 1U;
    ucReadyIdx = 0U;
    prvDownscaleNV12toRGB( pucY, pucUV, ulSrcWidth, ulSrcHeight, ucNnInputBuf[ 0 ] );
    SCB_CleanDCache_by_Addr( ( uint32_t * ) ucNnInputBuf[ 0 ],
                             ( int32_t ) ( AI_NN_WIDTH * AI_NN_HEIGHT * AI_NN_BPP ) );
    ( void ) xSemaphoreGive( xFrameReady );
}

/* ── NPU bring-up (from donor main.c).  AXISRAM3-6 clocks are enabled by
 *    FSBL/system init (RCC MEMENSR); those RAMs are reserved for the NPU
 *    activation pools by the linker map — see STM32N657X0HXQ_LRUN_kvs.ld. ── */
static void prvNpuEnable( void )
{
    __HAL_RCC_NPU_CLK_ENABLE();
    __HAL_RCC_NPU_FORCE_RESET();
    __HAL_RCC_NPU_RELEASE_RESET();

    npu_cache_enable();

    /* Grant the NPU the same RIF master/slave attributes the other bus
     * masters (DCMIPP/VENC) already have in this firmware. */
    __HAL_RCC_RIFSC_CLK_ENABLE();
    RIMC_MasterConfig_t xMaster = { 0 };
    xMaster.MasterCID = RIF_CID_1;
    xMaster.SecPriv   = RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV;
    HAL_RIF_RIMC_ConfigMasterAttributes( RIF_MASTER_INDEX_NPU, &xMaster );
    HAL_RIF_RISC_SetSlaveSecureAttributes( RIF_RISC_PERIPH_INDEX_NPU,
                                           RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV );
}

/* ── DCMIPP PIPE2 ───────────────────────────────────────────────────────── */
void AiDetection_PipeInit( int lSensorWidth, int lSensorHeight )
{
    CMW_DCMIPP_Conf_t xConf;
    uint32_t ulHwPitch;
    int ret;

    memset( &xConf, 0, sizeof( xConf ) );
    xConf.output_width  = AI_NN_WIDTH;
    xConf.output_height = AI_NN_HEIGHT;
    xConf.output_format = AI_NN_FORMAT;
    xConf.output_bpp    = AI_NN_BPP;
    /* ROOT-CAUSE FIX 2026-07-22 (dets=0 / solid-white NN input): with
     * CMW_Aspect_ratio_fit the middleware leaves PIPE2's CROP block
     * DISABLED (crop_conf stays zeroed -> DisableCrop), and on the shared
     * pipe1/pipe2 flow the branch then delivers saturated full-scale
     * samples (every byte 0xFF; probes/register dumps 2026-07-22, gamma
     * and P1-YUVCR pokes both eliminated).  The working donor
     * (x-cube-n6-ai-h264-usb-uvc) differs in exactly one register: its
     * manual_roi covers the FULL sensor, so crop is ENABLED with a
     * full-frame window while decimation/downsize ratios are identical.
     * Mirror that: manual_roi over the whole sensor. */
    xConf.mode          = CMW_Aspect_ratio_manual_roi;
    xConf.manual_conf.offset_x = 0;
    xConf.manual_conf.offset_y = 0;
    xConf.manual_conf.width    = ( uint32_t ) lSensorWidth;
    xConf.manual_conf.height   = ( uint32_t ) lSensorHeight;
    xConf.enable_swap   = 1;
    xConf.enable_gamma_conversion = 0;

    ret = CMW_CAMERA_SetPipeConfig( DCMIPP_PIPE2, &xConf, &ulHwPitch );
    configASSERT( ret == HAL_OK );
    configASSERT( ulHwPitch == ( uint32_t ) ( AI_NN_WIDTH * AI_NN_BPP ) );

    LogInfo( "[AI] PIPE2 configured %dx%d RGB888", AI_NN_WIDTH, AI_NN_HEIGHT );
}

/* PIPE2 is intentionally NOT started: its shared-ISP fork is dead on this
 * silicon (delivers only 0xFF regardless of every register setting — proven
 * on-board).  The NPU is instead fed from the working PIPE1 frame via
 * AiDetection_SubmitNV12(), called by the media task.  This stays a defined
 * symbol (the media path calls it) but does nothing. */
void AiDetection_PipeStart( void )
{
    LogInfo( "[AI] PIPE2 disabled (dead shared fork); NN fed from PIPE1 downscale" );
}

void AiDetection_PipeStop( void )
{
    if( ucPipeRunning != 0U )
    {
        ucPipeRunning = 0U;
        ( void ) CMW_CAMERA_Suspend( DCMIPP_PIPE2 );
    }
}

void AiDetection_FrameDoneISR( void )
{
    BaseType_t xWoken = pdFALSE;

    if( ( ucPipeRunning == 0U ) || ( xFrameReady == NULL ) )
    {
        return;
    }

    /* Completed buffer becomes ready; DCMIPP continues into the other. */
    ucReadyIdx   = ucCaptureIdx;
    /* DIAG 2026-07-23: count frames + snapshot the live shadow packer
     * address to confirm the buf0<->buf1 flip actually latches. */
    ulP2Frames++;
    ulP2CurAR = CMW_CAMERA_GetDCMIPPHandle()->Instance->P2CPPM0AR1;
    ucCaptureIdx = ( uint8_t ) ( 1U - ucCaptureIdx );
    ( void ) HAL_DCMIPP_PIPE_SetMemoryAddress( CMW_CAMERA_GetDCMIPPHandle(),
                                               DCMIPP_PIPE2,
                                               DCMIPP_MEMORY_ADDRESS_0,
                                               ( uint32_t ) ucNnInputBuf[ ucCaptureIdx ] );
    ( void ) xSemaphoreGiveFromISR( xFrameReady, &xWoken );
    portYIELD_FROM_ISR( xWoken );
}

/* ── Inference task ─────────────────────────────────────────────────────── */
static void prvRunInference( stai_network * pxNet )
{
    stai_return_code ret;

    do
    {
        ret = stai_network_run( pxNet, STAI_MODE_ASYNC );

        if( ret == STAI_RUNNING_WFE )
        {
            LL_ATON_OSAL_WFE();
        }
    } while( ( ret == STAI_RUNNING_WFE ) || ( ret == STAI_RUNNING_NO_WFE ) );

    ( void ) stai_ext_network_new_inference( pxNet );
}

/* NOR window arbitration (lfs_port_xspi.c): littlefs program/erase briefly
 * drops the XSPI2 memory-mapped window; hold this lock whenever the CPU or
 * NPU reads the weights region at 0x70380000. */
extern void vNorWindowLock( void );
extern void vNorWindowUnlock( void );

/* Raw (blocking, never-dropped) UART markers for the init path — the queued
 * logger drops lines under boot-time burst, which made AI init progress
 * unobservable.  Same bounded-spin register write as the [CAM]/[MP] prints
 * (mp_raw_putc in stm32_media_port.c — static there, so mirrored). */
extern void vPetWatchdog( void );
static void prvAiRawPutc( char c )
{
    for( uint32_t i = 0; i < 600000UL; i++ )
    {
        if( *( volatile uint32_t * ) 0x56000C1CUL & ( 1UL << 7 ) )
        {
            *( volatile uint32_t * ) 0x56000C28UL = ( uint32_t ) c;
            return;
        }
    }
    vPetWatchdog();
}
static void prvAiRaw( const char * pcStr )
{
    while( *pcStr != '\0' )
    {
        prvAiRawPutc( *pcStr++ );
    }
}

/* On failure: log and park the task (NOT configASSERT — its while(1) spin
 * burned 30-60% CPU alongside streaming when init hung).  Streaming must
 * survive any AI init failure.  The unlock is a harmless no-op when the
 * window lock is not held (mutex give by a non-owner just fails). */
#define AI_CHECK( cond, tag )                                        \
    do {                                                             \
        if( !( cond ) ) {                                            \
            LogError( "[AI] init failed at %s (ret=%d) - AI disabled", tag, ret ); \
            prvAiRaw( "[AI] PARKED: " );                             \
            prvAiRaw( tag );                                         \
            prvAiRaw( "\r\n" );                                      \
            vNorWindowUnlock();                                      \
            for( ; ; ) { vTaskSuspend( NULL ); }                     \
        }                                                            \
    } while( 0 )

static void prvAiTask( void * pvParam )
{
    stai_network_info xInfo;
    static od_pp_out_t xPpOut;                        /* detection results   */
    static od_yolov2_pp_static_param_t xPpParams;     /* postprocess config  */
    int ret;
    uint32_t ulInferences = 0U;
    uint32_t ulLastRunMs = 0U;
    TickType_t xLastLog = 0;
    TickType_t xLastInferStart = 0;

    ( void ) pvParam;

    /* The model weights + ec blobs live in NOR flash at 0x70380000 and are
     * read through the XSPI2 memory-mapped window by both the CPU (ec blob
     * copy during network init) and the NPU (weight fetch during inference).
     * The littlefs OSPI driver runs XSPI2 in INDIRECT command mode, which
     * may have torn the mapping down — probe before touching stai, because
     * init against an unmapped/garbage window hangs with no diagnostic. */
    prvAiRaw( "[AI] task>\r\n" );

    /* Hold the NOR window lock across probe + network init: the CPU reads
     * the ec blobs from flash here and the window must stay mapped. */
    vNorWindowLock();

    prvAiRaw( "[AI] lock ok\r\n" );

    LogInfo( "[AI] XSPI1 CR=%08lx XSPI2 CR=%08lx", XSPI1->CR, XSPI2->CR );

    /* Weight-fetch bandwidth check: SCLK = kernel / (prescaler+1).  Wants
     * kernel=200 MHz (HCLK) and prescaler=0; the SFDP driver parked SCLK at
     * 50 MHz single-wire before the 8LINES/octal-DTR switch. */
    {
        uint32_t ulKernelHz = HAL_RCCEx_GetPeriphCLKFreq( RCC_PERIPHCLK_XSPI2 );
        uint32_t ulPresc = ( XSPI2->DCR2 & XSPI_DCR2_PRESCALER_Msk ) >> XSPI_DCR2_PRESCALER_Pos;
        LogInfo( "[AI] XSPI2 kernel=%lu Hz presc=%lu sclk=%lu Hz",
                 ulKernelHz, ulPresc, ulKernelHz / ( ulPresc + 1U ) );
    }

    if( ( XSPI2->CR & XSPI_CR_FMODE_Msk ) != XSPI_CR_FMODE_Msk )
    {
        LogError( "[AI] XSPI2 not memory-mapped (FMODE=%lu) - weights at "
                  "0x70380000 unreachable; AI disabled",
                  ( XSPI2->CR & XSPI_CR_FMODE_Msk ) >> XSPI_CR_FMODE_Pos );
        prvAiRaw( "[AI] PARKED: fmode\r\n" );
        vNorWindowUnlock();
        for( ; ; ) { vTaskSuspend( NULL ); }
    }

    prvAiRaw( "[AI] probe>\r\n" );

    {
        volatile const uint32_t * pulW = ( volatile const uint32_t * ) 0x70380000UL;
        LogInfo( "[AI] weights probe: %08lx %08lx %08lx %08lx",
                 pulW[ 0 ], pulW[ 1 ], pulW[ 2 ], pulW[ 3 ] );
    }

    prvAiRaw( "[AI] rt_init>\r\n" );
    ret = stai_runtime_init();
    prvAiRaw( "[AI] rt_init<\r\n" );
    LogInfo( "[AI] stai_runtime_init ret=%d", ret );
    AI_CHECK( ret == STAI_SUCCESS, "runtime_init" );

    prvAiRaw( "[AI] net_init>\r\n" );
    ret = stai_network_init( ( stai_network * ) network_ctx );
    prvAiRaw( "[AI] net_init<\r\n" );
    LogInfo( "[AI] stai_network_init ret=%d", ret );
    AI_CHECK( ret == STAI_SUCCESS, "network_init" );

    prvAiRaw( "[AI] get_info>\r\n" );
    ret = stai_network_get_info( ( stai_network * ) network_ctx, &xInfo );
    prvAiRaw( "[AI] get_info<\r\n" );
    AI_CHECK( ret == STAI_SUCCESS, "get_info" );
    AI_CHECK( xInfo.n_inputs == 1, "n_inputs" );
    AI_CHECK( xInfo.outputs[ 0 ].size_bytes <= AI_OUT_BUFFER_MAX_BYTES, "out_size" );

    ret = app_postprocess_init( &xPpParams, &xInfo );
    AI_CHECK( ret == 0, "postprocess_init" );

    /* BUGFIX 2026-07-21: the yolov2 UF postprocess wrapper (unlike the
     * yolox/ssd/yolov8 ones) never assigns pOutBuff, so the NMS'd boxes
     * were memcpy'd to address 0 (silently ignored on N6) and the
     * top-conf telemetry loop never ran — ulLatestTopConfPct was
     * permanently 0.  Give the postprocess a real output array. */
    {
        static od_pp_outBuffer_t xPpBoxes[ AI_OD_YOLOV2_PP_MAX_BOXES_LIMIT ];
        xPpOut.pOutBuff = xPpBoxes;
    }

    vNorWindowUnlock();

    prvAiRaw( "[AI] ready\r\n" );

    LogInfo( "[AI] network ready: in %dx%dx%d, out %u bytes",
             AI_NN_WIDTH, AI_NN_HEIGHT, AI_NN_BPP,
             ( unsigned ) xInfo.outputs[ 0 ].size_bytes );

    ucAiReady = 1U;

    /* PIPE2 is started by the media path when the camera starts; until
     * then this loop just idles on the 1 s semaphore timeout. */

    for( ; ; )
    {
        stai_ptr xIn[ 1 ];
        stai_ptr xOut[ 1 ];

        if( xSemaphoreTake( xFrameReady, pdMS_TO_TICKS( 1000 ) ) != pdTRUE )
        {
            continue;
        }

        /* Rate-limiting is owned by AiDetection_SubmitNV12 (the media task
         * only downscales + signals at the inference cadence and never while
         * ucAiBusy is set), so every frame taken here is meant to be run —
         * do NOT 'continue' past this point without clearing ucAiBusy, or the
         * media task will never submit again. */

#if ( AI_BISECT_SKIP_INFERENCE == 1 )
        /* PIPE2 frames arrive and rotate; NPU stays idle.  Heartbeat shows
         * the pipe is alive so the A/B evidence is in the log. */
        ulInferences++;

        if( ( xTaskGetTickCount() - xLastLog ) > pdMS_TO_TICKS( 2000 ) )
        {
            xLastLog = xTaskGetTickCount();
            LogInfo( "[AI] BISECT pipe2-only: frames=%u (inference skipped)",
                     ( unsigned ) ulInferences );
        }

        xLastInferStart = xTaskGetTickCount();
        continue;
#endif /* AI_BISECT_SKIP_INFERENCE */

        /* BUGFIX 2026-07-22: the NPU reads the input through CACHEAXI,
         * but DCMIPP writes new frames to PSRAM BEHIND that cache — and
         * both 150KB input buffers fit inside it, so once cached the NPU
         * could infer on the first frames forever (dets=0 always, output
         * constant).  Drop the input range from the NPU cache so every
         * run refetches the fresh frame. */
        npu_cache_clean_invalidate_range( ( uint32_t ) ucNnInputBuf[ ucReadyIdx ],
                                          ( uint32_t ) ucNnInputBuf[ ucReadyIdx ] +
                                          ( AI_NN_WIDTH * AI_NN_HEIGHT * AI_NN_BPP ) );

        xIn[ 0 ] = ( stai_ptr ) ucNnInputBuf[ ucReadyIdx ];
        ret = stai_network_set_inputs( ( stai_network * ) network_ctx, xIn, 1 );
        AI_CHECK( ret == STAI_SUCCESS, "set_inputs" );

        SCB_InvalidateDCache_by_Addr( ( uint32_t * ) ucNnOutputBuf,
                                      ( int32_t ) xInfo.outputs[ 0 ].size_bytes );
        xOut[ 0 ] = ( stai_ptr ) ucNnOutputBuf;
        ret = stai_network_set_outputs( ( stai_network * ) network_ctx, xOut, 1 );
        AI_CHECK( ret == STAI_SUCCESS, "set_outputs" );

        if( ulInferences == 0U )
        {
            prvAiRaw( "[AI] frame0, run>\r\n" );
        }

        /* NPU fetches weights from the mapped NOR window during epochs —
         * exclude concurrent littlefs prog/erase which drops the window. */
        xLastInferStart = xTaskGetTickCount();
        vNorWindowLock();
        prvRunInference( ( stai_network * ) network_ctx );
        vNorWindowUnlock();
        ulLastRunMs = ( uint32_t ) ( ( xTaskGetTickCount() - xLastInferStart )
                                     * portTICK_PERIOD_MS );
        ulInferences++;

        /* BUGFIX 2026-07-22: the pre-run invalidate above evicts dirty
         * lines, but the M55 can speculatively RE-fill output lines while
         * the 30-100 ms inference runs — the postprocess then reads
         * pre-inference stale bytes (zeros -> sigmoid 0.5 < 0.6 conf
         * threshold) and dets stayed 0 forever.  Invalidate again AFTER
         * the NPU has written, immediately before the CPU reads. */
        SCB_InvalidateDCache_by_Addr( ( uint32_t * ) ucNnOutputBuf,
                                      ( int32_t ) xInfo.outputs[ 0 ].size_bytes );

        if( ulInferences == 1U )
        {
            /* First-inference timing: the NoC-stall pass/fail evidence from
             * the weight-fetch bring-up era; cheap, keep it. */
            LogInfo( "[AI] first inference: %lu ms", ulLastRunMs );
        }

        ret = app_postprocess_run( ( void * [] ) { ucNnOutputBuf }, 1,
                                   &xPpOut, &xPpParams );

        /* Telemetry snapshot (Phase 3): count + best confidence in percent. */
        {
            float xTopConf = 0.0f;
            int32_t lI;

            for( lI = 0; ( lI < xPpOut.nb_detect ) && ( xPpOut.pOutBuff != NULL ); lI++ )
            {
                if( xPpOut.pOutBuff[ lI ].conf > xTopConf )
                {
                    xTopConf = xPpOut.pOutBuff[ lI ].conf;
                }
            }

            lLatestDetections  = ( ret == 0 ) ? xPpOut.nb_detect : 0;
            ulLatestTopConfPct = ( uint32_t ) ( xTopConf * 100.0f );
            ulLatestInferMs    = ulLastRunMs;
            ulTotalInferences  = ulInferences;
        }

        /* Box snapshot (multi-word — needs the critical section, unlike the
         * scalar telemetry) + LCD overlay update at the inference rate. */
        {
            LcdBox_t xBoxes[ LCD_PREVIEW_MAX_BOXES ];
            uint32_t ulCount = 0U;
            int32_t  lI;

            if( ( ret == 0 ) && ( xPpOut.pOutBuff != NULL ) )
            {
                for( lI = 0; ( lI < xPpOut.nb_detect ) &&
                             ( ulCount < LCD_PREVIEW_MAX_BOXES ); lI++ )
                {
                    xBoxes[ ulCount ].fXCenter = xPpOut.pOutBuff[ lI ].x_center;
                    xBoxes[ ulCount ].fYCenter = xPpOut.pOutBuff[ lI ].y_center;
                    xBoxes[ ulCount ].fWidth   = xPpOut.pOutBuff[ lI ].width;
                    xBoxes[ ulCount ].fHeight  = xPpOut.pOutBuff[ lI ].height;
                    xBoxes[ ulCount ].fConf    = xPpOut.pOutBuff[ lI ].conf;
                    ulCount++;
                }
            }

            taskENTER_CRITICAL();
            ulBoxSnapCount = ulCount;
            ( void ) memcpy( xBoxSnap, xBoxes, ulCount * sizeof( LcdBox_t ) );
            taskEXIT_CRITICAL();

            LcdPreview_UpdateOverlay( xBoxes, ulCount );
        }

        /* Phase 3: throttled logging of detection count (telemetry Phase 4). */
        if( ( xTaskGetTickCount() - xLastLog ) > pdMS_TO_TICKS( 2000 ) )
        {
            xLastLog = xTaskGetTickCount();
            LogInfo( "[AI] inferences=%u dets=%d pp_ret=%d last=%lums",
                     ( unsigned ) ulInferences,
                     ( int ) xPpOut.nb_detect, ret, ulLastRunMs );
        }

        /* NN input buffer is free again — let the media task downscale the
         * next PIPE1 frame into it. */
        ucAiBusy = 0U;
    }
}

void AiDetection_Init( void )
{
    extern volatile uint32_t g_npu_cache_state;

    prvNpuEnable();

    prvAiRaw( "[AI] cache state=" );
    prvAiRawPutc( ( char ) ( '0' + ( g_npu_cache_state & 0xFU ) ) );
    prvAiRaw( "\r\n" );

    xFrameReady = xSemaphoreCreateBinary();
    configASSERT( xFrameReady != NULL );

    if( xTaskCreate( prvAiTask,
                     "AiDetect",
                     4096,
                     NULL,
                     tskIDLE_PRIORITY + 1,
                     NULL ) != pdPASS )
    {
        LogError( "[AI] failed to create inference task" );
    }
}

#endif /* ENABLE_AI_DETECTION */
