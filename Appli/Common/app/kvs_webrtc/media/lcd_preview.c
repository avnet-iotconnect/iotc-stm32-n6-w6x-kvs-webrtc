/*
 * lcd_preview.c — RK050HR18 800x480 LCD preview (see lcd_preview.h).
 *
 * Layer 1: FlexYUV semi-planar (NV12 4:2:0) scanout of the media
 *          pipeline's ping-pong buffers, 640x480 centered at (80,0).
 *          Flipped per camera frame from the DCMIPP frame ISR via
 *          HAL_LTDC_SetSemiPlanarAddress_NoReload + vblank reload.
 * Layer 2: ARGB4444 overlay, double-buffered in PSRAM, CPU-drawn
 *          detection boxes at the AI inference rate.
 *
 * Pixel clock: IC16 = PLL4(1600 MHz)/64 = 25 MHz exactly (PLL4 is set up
 * by the FSBL and otherwise unused).  Panel timings per RK050HR18:
 * HS/HBP/HFP = VS/VBP/VFP = 4.
 *
 * RIF: LTDC bus masters + peripheral get the same CID1 SEC|PRIV
 * attributes as DCMIPP/VENC (pattern from SystemIsolation_Config and
 * ai_detection.c prvNpuEnable).  No RISAF is configured anywhere in this
 * project, so LTDC reads of PSRAM pass like DCMIPP/VENC DMA already does.
 */

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "stm32n6xx_hal.h"
#include "lcd_preview.h"

/* ── Panel geometry ─────────────────────────────────────────────────────── */
#define LCD_W               ( 800U )
#define LCD_H               ( 480U )
#define LCD_HSYNC           ( 4U )
#define LCD_HBP             ( 4U )
#define LCD_HFP             ( 4U )
#define LCD_VSYNC           ( 4U )
#define LCD_VBP             ( 4U )
#define LCD_VFP             ( 4U )

/* Video window: 640x480 NV12 centered on the 800-wide panel. */
#define VID_W               ( 640U )
#define VID_H               ( 480U )
#define VID_X0              ( ( LCD_W - VID_W ) / 2U )   /* 80 */
#define VID_Y0              ( 0U )

/* Overlay: same window as the video (saves LTDC fetch bandwidth). */
#define OVL_W               VID_W
#define OVL_H               VID_H
#define OVL_BYTES           ( OVL_W * OVL_H * 2U )       /* ARGB4444 */

/* ARGB4444 colors (A|R|G|B nibbles) */
#define OVL_TRANSPARENT     ( 0x0000U )
#define OVL_BOX_COLOR       ( 0xF0F0U )                  /* opaque green  */
#define OVL_BOX_THICKNESS   ( 3U )

static LTDC_HandleTypeDef xLtdc;

static uint8_t  ucLcdReady   = 0U;
static uint8_t  ucVideoLive  = 0U;       /* Layer 1 configured+enabled     */
static uint8_t  ucOvlDrawIdx = 0U;       /* overlay buffer being drawn     */
static volatile uint32_t ulUnderruns = 0U;

/* Reused by the ISR flip — only the two addresses change per frame. */
static LTDC_LayerFlexYUVSemiPlanarTypeDef xVideoLayer;

/* Overlay double buffer — PSRAM, like every other big media buffer. */
static uint16_t usOvlBuf[ 2 ][ OVL_W * OVL_H ]
    __attribute__(( section( ".psram_bss" ), aligned( 32 ) ));

/* ── Init ───────────────────────────────────────────────────────────────── */

static void prvLcdClockConfig( void )
{
    RCC_PeriphCLKInitTypeDef xClk = { 0 };

    /* LTDC kernel clock = IC16 = PLL4 / 64 = 1600/64 = 25.000 MHz.
     * Refresh = 25 MHz / ((800+12)*(480+12)) = 62.6 Hz.               */
    xClk.PeriphClockSelection                        = RCC_PERIPHCLK_LTDC;
    xClk.LtdcClockSelection                          = RCC_LTDCCLKSOURCE_IC16;
    xClk.ICSelection[ RCC_IC16 ].ClockSelection      = RCC_ICCLKSOURCE_PLL4;
    xClk.ICSelection[ RCC_IC16 ].ClockDivider        = 64;
    ( void ) HAL_RCCEx_PeriphCLKConfig( &xClk );
}

static void prvLcdRifConfig( void )
{
    /* Same pattern as SystemIsolation_Config (DCMIPP/VENC) and
     * ai_detection prvNpuEnable (NPU): CID1, secure, privileged. */
    RIMC_MasterConfig_t xMaster = { 0 };

    __HAL_RCC_RIFSC_CLK_ENABLE();

    xMaster.MasterCID = RIF_CID_1;
    xMaster.SecPriv   = RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV;
    HAL_RIF_RIMC_ConfigMasterAttributes( RIF_MASTER_INDEX_LTDC1, &xMaster );
    HAL_RIF_RIMC_ConfigMasterAttributes( RIF_MASTER_INDEX_LTDC2, &xMaster );

    HAL_RIF_RISC_SetSlaveSecureAttributes( RIF_RISC_PERIPH_INDEX_LTDC,
                                           RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV );
    HAL_RIF_RISC_SetSlaveSecureAttributes( RIF_RISC_PERIPH_INDEX_LTDCL1,
                                           RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV );
    HAL_RIF_RISC_SetSlaveSecureAttributes( RIF_RISC_PERIPH_INDEX_LTDCL2,
                                           RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV );
}

/* Pin map: STM32N6570-DK MB1860A, RK050HR18 parallel RGB666+ panel.
 * (Same map as the BSP driver; PA15/PB4 are JTAG pins — SWD on
 * PA13/PA14 is unaffected.) */
static void prvLcdGpioInit( void )
{
    GPIO_InitTypeDef xGpio = { 0 };

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();
    __HAL_RCC_GPIOH_CLK_ENABLE();
    __HAL_RCC_GPIOQ_CLK_ENABLE();

    /* TrustZone pin attributes — mirror the app's pattern in main.c. */
    HAL_GPIO_ConfigPinAttributes( GPIOA, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7 |
                                         GPIO_PIN_8 | GPIO_PIN_15,
                                  GPIO_PIN_SEC | GPIO_PIN_NPRIV );
    HAL_GPIO_ConfigPinAttributes( GPIOB, GPIO_PIN_2 | GPIO_PIN_4 | GPIO_PIN_11 | GPIO_PIN_12 |
                                         GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15,
                                  GPIO_PIN_SEC | GPIO_PIN_NPRIV );
    HAL_GPIO_ConfigPinAttributes( GPIOD, GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_15,
                                  GPIO_PIN_SEC | GPIO_PIN_NPRIV );
    HAL_GPIO_ConfigPinAttributes( GPIOE, GPIO_PIN_1 | GPIO_PIN_11,
                                  GPIO_PIN_SEC | GPIO_PIN_NPRIV );
    HAL_GPIO_ConfigPinAttributes( GPIOG, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_6 | GPIO_PIN_8 |
                                         GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13,
                                  GPIO_PIN_SEC | GPIO_PIN_NPRIV );
    HAL_GPIO_ConfigPinAttributes( GPIOH, GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_6,
                                  GPIO_PIN_SEC | GPIO_PIN_NPRIV );
    HAL_GPIO_ConfigPinAttributes( GPIOQ, GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_6,
                                  GPIO_PIN_SEC | GPIO_PIN_NPRIV );

    xGpio.Mode      = GPIO_MODE_AF_PP;
    xGpio.Pull      = GPIO_NOPULL;
    xGpio.Speed     = GPIO_SPEED_FREQ_HIGH;
    xGpio.Alternate = GPIO_AF14_LCD;

    /* PA: G3 G2 B7 B1 B6 R5 */
    xGpio.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_15;
    HAL_GPIO_Init( GPIOA, &xGpio );

    /* PB: B2 R3 G6 G5 CLK HSYNC G4 */
    xGpio.Pin = GPIO_PIN_2 | GPIO_PIN_4 | GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13 |
                GPIO_PIN_14 | GPIO_PIN_15;
    HAL_GPIO_Init( GPIOB, &xGpio );

    /* PD: R7 R1 R2 */
    xGpio.Pin = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_15;
    HAL_GPIO_Init( GPIOD, &xGpio );

    /* PE: VSYNC */
    xGpio.Pin = GPIO_PIN_11;
    HAL_GPIO_Init( GPIOE, &xGpio );

    /* PG: R0 G1 B3 G7 R6 G0 */
    xGpio.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_6 | GPIO_PIN_8 | GPIO_PIN_11 | GPIO_PIN_12;
    HAL_GPIO_Init( GPIOG, &xGpio );

    /* PH: B4 R4 B5 */
    xGpio.Pin = GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_6;
    HAL_GPIO_Init( GPIOH, &xGpio );

    /* Control outputs */
    xGpio.Mode      = GPIO_MODE_OUTPUT_PP;
    xGpio.Alternate = 0;

    xGpio.Pin = GPIO_PIN_1;                      /* PE1  = panel NRST      */
    HAL_GPIO_Init( GPIOE, &xGpio );
    xGpio.Pin = GPIO_PIN_13;                     /* PG13 = DE (fixed high) */
    HAL_GPIO_Init( GPIOG, &xGpio );
    xGpio.Pin = GPIO_PIN_3 | GPIO_PIN_6;         /* PQ3 ON/OFF, PQ6 BL     */
    HAL_GPIO_Init( GPIOQ, &xGpio );

    /* Release panel reset, then power/backlight on. */
    HAL_GPIO_WritePin( GPIOE, GPIO_PIN_1, GPIO_PIN_SET );
    vTaskDelay( pdMS_TO_TICKS( 2 ) );
    HAL_GPIO_WritePin( GPIOQ, GPIO_PIN_3, GPIO_PIN_SET );    /* LCD on     */
    HAL_GPIO_WritePin( GPIOG, GPIO_PIN_13, GPIO_PIN_SET );   /* DE high    */
    HAL_GPIO_WritePin( GPIOQ, GPIO_PIN_6, GPIO_PIN_SET );    /* backlight  */
}

void LcdPreview_Init( void )
{
    LTDC_LayerCfgTypeDef xOvl = { 0 };

#if ( LCD_PREVIEW_ENABLE == 0 )
    return;               /* A/B switch: LCD fully inert, see lcd_preview.h */
#endif

    if( ucLcdReady != 0U )
    {
        return;
    }

    prvLcdClockConfig();
    prvLcdRifConfig();

    __HAL_RCC_LTDC_CLK_ENABLE();
    __HAL_RCC_LTDC_FORCE_RESET();
    __HAL_RCC_LTDC_RELEASE_RESET();

    prvLcdGpioInit();

    /* RK050HR18 timings, polarities per the DK BSP (AL/AL/AL/IPC). */
    xLtdc.Instance                = LTDC;
    xLtdc.Init.HSPolarity         = LTDC_HSPOLARITY_AL;
    xLtdc.Init.VSPolarity         = LTDC_VSPOLARITY_AL;
    xLtdc.Init.DEPolarity         = LTDC_DEPOLARITY_AL;
    xLtdc.Init.PCPolarity         = LTDC_PCPOLARITY_IPC;
    xLtdc.Init.HorizontalSync     = LCD_HSYNC - 1U;
    xLtdc.Init.AccumulatedHBP     = LCD_HSYNC + LCD_HBP - 1U;
    xLtdc.Init.AccumulatedActiveW = LCD_HSYNC + LCD_HBP + LCD_W - 1U;
    xLtdc.Init.TotalWidth         = LCD_HSYNC + LCD_HBP + LCD_W + LCD_HFP - 1U;
    xLtdc.Init.VerticalSync       = LCD_VSYNC - 1U;
    xLtdc.Init.AccumulatedVBP     = LCD_VSYNC + LCD_VBP - 1U;
    xLtdc.Init.AccumulatedActiveH = LCD_VSYNC + LCD_VBP + LCD_H - 1U;
    xLtdc.Init.TotalHeigh         = LCD_VSYNC + LCD_VBP + LCD_H + LCD_VFP - 1U;
    xLtdc.Init.Backcolor.Red      = 0;
    xLtdc.Init.Backcolor.Green    = 0;
    xLtdc.Init.Backcolor.Blue     = 0;

    if( HAL_LTDC_Init( &xLtdc ) != HAL_OK )
    {
        return;                       /* leave ucLcdReady 0 — all API no-op */
    }

    /* Layer 1 (video) template — addresses patched per frame in the ISR.
     * FlexYUV semi-planar = NV12: full-range BT.601, U before V. */
    xVideoLayer.Layer.WindowX0        = VID_X0;
    xVideoLayer.Layer.WindowX1        = VID_X0 + VID_W;
    xVideoLayer.Layer.WindowY0        = VID_Y0;
    xVideoLayer.Layer.WindowY1        = VID_Y0 + VID_H;
    xVideoLayer.Layer.Alpha           = 255;
    xVideoLayer.Layer.Alpha0          = 0;
    xVideoLayer.Layer.BlendingFactor1 = LTDC_BLENDING_FACTOR1_PAxCA;
    xVideoLayer.Layer.BlendingFactor2 = LTDC_BLENDING_FACTOR2_PAxCA;
    xVideoLayer.Layer.ImageWidth      = VID_W;
    xVideoLayer.Layer.ImageHeight     = VID_H;
    xVideoLayer.FlexYUV.YUVOrder         = LTDC_YUV_ORDER_LUMINANCE_FIRST;
    xVideoLayer.FlexYUV.LuminanceOrder   = LTDC_YUV_LUMINANCE_ORDER_ODD_FIRST;
    xVideoLayer.FlexYUV.ChrominanceOrder = LTDC_YUV_CHROMIANCE_ORDER_U_FIRST;
    xVideoLayer.FlexYUV.LuminanceRescale = LTDC_YUV_LUMINANCE_RESCALE_DISABLE;
    xVideoLayer.ColorConverter           = LTDC_YUV2RGBCONVERTOR_BT601_FULL_RANGE;

    /* Layer 2 (overlay): ARGB4444, transparent until first draw. */
    ( void ) memset( usOvlBuf, 0, sizeof( usOvlBuf ) );
    SCB_CleanDCache_by_Addr( ( uint32_t * ) usOvlBuf, ( int32_t ) sizeof( usOvlBuf ) );

    xOvl.WindowX0        = VID_X0;
    xOvl.WindowX1        = VID_X0 + OVL_W;
    xOvl.WindowY0        = VID_Y0;
    xOvl.WindowY1        = VID_Y0 + OVL_H;
    xOvl.PixelFormat     = LTDC_PIXEL_FORMAT_ARGB4444;
    xOvl.Alpha           = 255;
    xOvl.Alpha0          = 0;
    xOvl.BlendingFactor1 = LTDC_BLENDING_FACTOR1_PAxCA;
    xOvl.BlendingFactor2 = LTDC_BLENDING_FACTOR2_PAxCA;
    xOvl.FBStartAdress   = ( uint32_t ) &usOvlBuf[ 0 ][ 0 ];
    xOvl.ImageWidth      = OVL_W;
    xOvl.ImageHeight     = OVL_H;

    if( HAL_LTDC_ConfigLayer( &xLtdc, &xOvl, LTDC_LAYER_2 ) != HAL_OK )
    {
        return;
    }

    ucOvlDrawIdx = 1U;                /* buffer 0 on screen, draw into 1 */
    ucLcdReady   = 1U;
}

/* ── Video layer flip (DCMIPP frame ISR) ────────────────────────────────── */

void LcdPreview_ShowFrameISR( uint8_t * pucY, uint8_t * pucUV )
{
    if( ( ucLcdReady == 0U ) || ( pucY == NULL ) || ( pucUV == NULL ) )
    {
        return;
    }

    xVideoLayer.YUVSemiPlanarAddress.YAddress  = ( uint32_t ) pucY;
    xVideoLayer.YUVSemiPlanarAddress.UVAddress = ( uint32_t ) pucUV;

    if( ucVideoLive == 0U )
    {
        /* First frame of a session: full layer config (immediate reload,
         * enables the layer). */
        if( HAL_LTDC_ConfigLayerFlexYUVSemiPlanar( &xLtdc, &xVideoLayer,
                                                   LTDC_LAYER_1 ) == HAL_OK )
        {
            ucVideoLive = 1U;
        }
    }
    else
    {
        /* Steady state: patch the two plane addresses, latch at vblank.
         * HAL_BUSY (rare collision with a task-side overlay flip) just
         * skips this frame's flip — the next frame retries. */
        if( HAL_LTDC_SetSemiPlanarAddress_NoReload( &xLtdc, &xVideoLayer,
                                                    LTDC_LAYER_1 ) == HAL_OK )
        {
            ( void ) HAL_LTDC_ReloadLayer( &xLtdc, LTDC_RELOAD_VERTICAL_BLANKING,
                                           LTDC_LAYER_1 );
        }
    }
}

void LcdPreview_Blank( void )
{
    if( ( ucLcdReady == 0U ) || ( ucVideoLive == 0U ) )
    {
        return;
    }

    __HAL_LTDC_LAYER_DISABLE( &xLtdc, LTDC_LAYER_1 );
    ( void ) HAL_LTDC_ReloadLayer( &xLtdc, LTDC_RELOAD_VERTICAL_BLANKING,
                                   LTDC_LAYER_1 );
    ucVideoLive = 0U;                 /* next session re-configures layer 1 */
}

/* ── Overlay ────────────────────────────────────────────────────────────── */

static void prvOvlFillRect( uint16_t * pusBuf, int32_t lX, int32_t lY,
                            int32_t lW, int32_t lH, uint16_t usColor )
{
    int32_t lRow;
    int32_t lCol;

    if( lX < 0 ) { lW += lX; lX = 0; }
    if( lY < 0 ) { lH += lY; lY = 0; }
    if( lX + lW > ( int32_t ) OVL_W ) { lW = ( int32_t ) OVL_W - lX; }
    if( lY + lH > ( int32_t ) OVL_H ) { lH = ( int32_t ) OVL_H - lY; }

    for( lRow = 0; lRow < lH; lRow++ )
    {
        uint16_t * pusLine = &pusBuf[ ( lY + lRow ) * ( int32_t ) OVL_W + lX ];

        for( lCol = 0; lCol < lW; lCol++ )
        {
            pusLine[ lCol ] = usColor;
        }
    }
}

static void prvOvlDrawBoxOutline( uint16_t * pusBuf, int32_t lX, int32_t lY,
                                  int32_t lW, int32_t lH, uint16_t usColor )
{
    const int32_t lT = ( int32_t ) OVL_BOX_THICKNESS;

    prvOvlFillRect( pusBuf, lX,          lY,          lW, lT, usColor );  /* top    */
    prvOvlFillRect( pusBuf, lX,          lY + lH - lT, lW, lT, usColor ); /* bottom */
    prvOvlFillRect( pusBuf, lX,          lY,          lT, lH, usColor );  /* left   */
    prvOvlFillRect( pusBuf, lX + lW - lT, lY,          lT, lH, usColor ); /* right  */
}

void LcdPreview_UpdateOverlay( const LcdBox_t * pxBoxes, uint32_t ulCount )
{
    uint16_t * pusBuf;
    uint32_t   ulIdx;

    if( ucLcdReady == 0U )
    {
        return;
    }

    pusBuf = &usOvlBuf[ ucOvlDrawIdx ][ 0 ];

    ( void ) memset( pusBuf, 0, OVL_BYTES );

    if( ulCount > LCD_PREVIEW_MAX_BOXES )
    {
        ulCount = LCD_PREVIEW_MAX_BOXES;
    }

    for( ulIdx = 0U; ulIdx < ulCount; ulIdx++ )
    {
        const LcdBox_t * pxB = &pxBoxes[ ulIdx ];

        /* YOLO center/size (normalized) → pixel corner rect, clamped.
         * Both DCMIPP pipes image the identical full-sensor FOV, so
         * normalized NN coords map 1:1 onto the 640x480 video. */
        int32_t lW = ( int32_t )( pxB->fWidth  * ( float ) VID_W );
        int32_t lH = ( int32_t )( pxB->fHeight * ( float ) VID_H );
        int32_t lX = ( int32_t )( ( pxB->fXCenter - pxB->fWidth  / 2.0f ) * ( float ) VID_W );
        int32_t lY = ( int32_t )( ( pxB->fYCenter - pxB->fHeight / 2.0f ) * ( float ) VID_H );

        if( lW < ( int32_t )( 2U * OVL_BOX_THICKNESS ) ) { lW = ( int32_t )( 2U * OVL_BOX_THICKNESS ); }
        if( lH < ( int32_t )( 2U * OVL_BOX_THICKNESS ) ) { lH = ( int32_t )( 2U * OVL_BOX_THICKNESS ); }
        if( lX < 0 ) { lX = 0; }
        if( lY < 0 ) { lY = 0; }
        if( lX + lW > ( int32_t ) OVL_W ) { lW = ( int32_t ) OVL_W - lX; }
        if( lY + lH > ( int32_t ) OVL_H ) { lH = ( int32_t ) OVL_H - lY; }

        prvOvlDrawBoxOutline( pusBuf, lX, lY, lW, lH, OVL_BOX_COLOR );
    }

    /* People count pips, top-left: one filled square per detection. */
    for( ulIdx = 0U; ulIdx < ulCount; ulIdx++ )
    {
        prvOvlFillRect( pusBuf, 8 + ( int32_t )( ulIdx * 18U ), 8, 12, 12,
                        OVL_BOX_COLOR );
    }

    /* PSRAM buffers are cacheable — push the pixels out before scanout. */
    SCB_CleanDCache_by_Addr( ( uint32_t * ) pusBuf, ( int32_t ) OVL_BYTES );

    if( HAL_LTDC_SetAddress_NoReload( &xLtdc, ( uint32_t ) pusBuf,
                                      LTDC_LAYER_2 ) == HAL_OK )
    {
        ( void ) HAL_LTDC_ReloadLayer( &xLtdc, LTDC_RELOAD_VERTICAL_BLANKING,
                                       LTDC_LAYER_2 );
        ucOvlDrawIdx ^= 1U;
    }
}

/* ── Diagnostics ────────────────────────────────────────────────────────── */

uint32_t LcdPreview_Underruns( void )
{
    if( ucLcdReady == 0U )
    {
        return 0U;
    }

    if( __HAL_LTDC_GET_FLAG( &xLtdc, LTDC_FLAG_FU ) != 0U )
    {
        __HAL_LTDC_CLEAR_FLAG( &xLtdc, LTDC_FLAG_FU );
        ulUnderruns++;
    }

    if( __HAL_LTDC_GET_FLAG( &xLtdc, LTDC_FLAG_TE ) != 0U )
    {
        __HAL_LTDC_CLEAR_FLAG( &xLtdc, LTDC_FLAG_TE );
        ulUnderruns++;
    }

    return ulUnderruns;
}
