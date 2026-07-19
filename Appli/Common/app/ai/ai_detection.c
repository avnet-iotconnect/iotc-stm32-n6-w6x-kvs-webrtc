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

#include "ai_detection.h"

/* ── Model geometry ─────────────────────────────────────────────────────── */
#define AI_NN_WIDTH    STAI_NETWORK_IN_1_WIDTH
#define AI_NN_HEIGHT   STAI_NETWORK_IN_1_HEIGHT
#define AI_NN_BPP      3
#define AI_NN_FORMAT   DCMIPP_PIXEL_PACKER_FORMAT_RGB888_YUV444_1

#define AI_OUT_BUFFER_MAX_BYTES   ( 64U * 1024U )

#if defined ( __GNUC__ )
    #define ALIGN_32    __attribute__((aligned(32)))
    #define IN_PSRAM    __attribute__((section(".psram_bss")))
#endif

/* ── Buffers ────────────────────────────────────────────────────────────── */
static uint8_t ucNnInputBuf[ 2 ][ AI_NN_WIDTH * AI_NN_HEIGHT * AI_NN_BPP ] ALIGN_32 IN_PSRAM;
static uint8_t ucNnOutputBuf[ AI_OUT_BUFFER_MAX_BYTES ] ALIGN_32 IN_PSRAM;
static uint8_t network_ctx[ STAI_NETWORK_CONTEXT_SIZE ] ALIGN_32;

/* Double-buffer state: DCMIPP writes ucNnInputBuf[ucCaptureIdx]; on frame
 * done the ISR flips the index, points PIPE2 at the fresh buffer and gives
 * the semaphore so the task processes the completed one. */
static volatile uint8_t  ucCaptureIdx  = 0U;
static volatile uint8_t  ucReadyIdx    = 0U;
static SemaphoreHandle_t xFrameReady   = NULL;
static volatile uint8_t  ucPipeRunning = 0U;

/* ── NPU bring-up (from donor main.c; RAMCFG for AXISRAM3-6 is already
 *    done by this firmware's system init — repeating it is harmless) ───── */
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
    xConf.mode          = CMW_Aspect_ratio_fit;
    xConf.enable_swap   = 1;
    xConf.enable_gamma_conversion = 0;

    ( void ) lSensorWidth;
    ( void ) lSensorHeight;

    ret = CMW_CAMERA_SetPipeConfig( DCMIPP_PIPE2, &xConf, &ulHwPitch );
    configASSERT( ret == HAL_OK );
    configASSERT( ulHwPitch == ( uint32_t ) ( AI_NN_WIDTH * AI_NN_BPP ) );

    LogInfo( "[AI] PIPE2 configured %dx%d RGB888", AI_NN_WIDTH, AI_NN_HEIGHT );
}

void AiDetection_PipeStart( void )
{
    int ret;

    ucCaptureIdx = 0U;
    ret = CMW_CAMERA_Start( DCMIPP_PIPE2, ucNnInputBuf[ 0 ], CMW_MODE_CONTINUOUS );
    configASSERT( ret == HAL_OK );
    ucPipeRunning = 1U;
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

static void prvAiTask( void * pvParam )
{
    stai_network_info xInfo;
    static od_pp_out_t xPpOut;                        /* detection results   */
    static od_yolov2_pp_static_param_t xPpParams;     /* postprocess config  */
    int ret;
    uint32_t ulInferences = 0U;
    TickType_t xLastLog = 0;

    ( void ) pvParam;

    ret = stai_runtime_init();
    configASSERT( ret == STAI_SUCCESS );
    ret = stai_network_init( ( stai_network * ) network_ctx );
    configASSERT( ret == STAI_SUCCESS );
    ret = stai_network_get_info( ( stai_network * ) network_ctx, &xInfo );
    configASSERT( ret == STAI_SUCCESS );
    configASSERT( xInfo.n_inputs == 1 );
    configASSERT( xInfo.outputs[ 0 ].size_bytes <= AI_OUT_BUFFER_MAX_BYTES );

    ret = app_postprocess_init( &xPpParams, &xInfo );
    configASSERT( ret == 0 );

    LogInfo( "[AI] network ready: in %dx%dx%d, out %u bytes",
             AI_NN_WIDTH, AI_NN_HEIGHT, AI_NN_BPP,
             ( unsigned ) xInfo.outputs[ 0 ].size_bytes );

    AiDetection_PipeStart();

    for( ; ; )
    {
        stai_ptr xIn[ 1 ];
        stai_ptr xOut[ 1 ];

        if( xSemaphoreTake( xFrameReady, pdMS_TO_TICKS( 1000 ) ) != pdTRUE )
        {
            continue;
        }

        /* Input written by DCMIPP, read by NPU — no CPU cache interaction.
         * Output written by NPU, read by CPU — invalidate before use. */
        xIn[ 0 ] = ( stai_ptr ) ucNnInputBuf[ ucReadyIdx ];
        ret = stai_network_set_inputs( ( stai_network * ) network_ctx, xIn, 1 );
        configASSERT( ret == STAI_SUCCESS );

        SCB_InvalidateDCache_by_Addr( ( uint32_t * ) ucNnOutputBuf,
                                      ( int32_t ) xInfo.outputs[ 0 ].size_bytes );
        xOut[ 0 ] = ( stai_ptr ) ucNnOutputBuf;
        ret = stai_network_set_outputs( ( stai_network * ) network_ctx, xOut, 1 );
        configASSERT( ret == STAI_SUCCESS );

        prvRunInference( ( stai_network * ) network_ctx );
        ulInferences++;

        ret = app_postprocess_run( ( void * [] ) { ucNnOutputBuf }, 1,
                                   &xPpOut, &xPpParams );

        /* Phase 3: throttled logging of detection count (telemetry Phase 4). */
        if( ( xTaskGetTickCount() - xLastLog ) > pdMS_TO_TICKS( 2000 ) )
        {
            xLastLog = xTaskGetTickCount();
            LogInfo( "[AI] inferences=%u dets=%d pp_ret=%d",
                     ( unsigned ) ulInferences,
                     ( int ) xPpOut.nb_detect, ret );
        }
    }
}

void AiDetection_Init( void )
{
    prvNpuEnable();

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
