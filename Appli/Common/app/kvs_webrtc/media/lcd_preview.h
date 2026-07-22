/*
 * lcd_preview.h — on-board RK050HR18 800x480 LCD preview of the KVS media
 * pipeline, with AI detection overlay.
 *
 * Zero-copy architecture: LTDC Layer 1 scans out the SAME NV12 640x480
 * ping-pong buffers that DCMIPP writes and VENC encodes (FlexYUV
 * semi-planar 4:2:0, BT.601 full-range conversion in the LTDC).  The
 * validated streaming pipeline is untouched — the LCD is a read-only
 * observer.  Layer 2 is an ARGB4444 overlay for detection boxes, updated
 * at the AI inference rate.
 */

#ifndef LCD_PREVIEW_H
#define LCD_PREVIEW_H

#include <stdint.h>

/* Master switch (A/B debugging): 0 = LcdPreview_Init no-ops, every other
 * call no-ops via the ready flag — byte-identical media/network behavior
 * to a build without the LCD.  2026-07-22: first on-panel build showed
 * total relay-TX starvation (e=11 stalls on large frames, RX fine) —
 * suspects are panel/backlight power draw vs LCD-bus EMI onto the W6X
 * SPI vs software; this switch is discriminator step 1. */
#define LCD_PREVIEW_ENABLE    ( 0 )

/* One detection box, normalized coordinates (0..1) relative to the video
 * frame; x/y are the box CENTER (YOLO convention). */
typedef struct
{
    float fXCenter;
    float fYCenter;
    float fWidth;
    float fHeight;
    float fConf;
} LcdBox_t;

#define LCD_PREVIEW_MAX_BOXES    ( 8U )

/* Full bring-up: clocks (IC16 = PLL4/64 = 25 MHz), RIF, GPIO, LTDC,
 * overlay layer.  Requires PSRAM up (overlay buffers live there) and the
 * scheduler running.  Idempotent. */
void LcdPreview_Init( void );

/* ISR-context: point Layer 1 at the just-completed NV12 frame (Y plane +
 * interleaved UV plane) and latch at next vertical blanking.  First call
 * configures and enables the layer. */
void LcdPreview_ShowFrameISR( uint8_t * pucY, uint8_t * pucUV );

/* Hide the video layer (session stopped / camera stopping).  Task ctx. */
void LcdPreview_Blank( void );

/* Redraw the overlay with the given boxes and flip it.  Task context
 * (called from the AI task after each inference).  ulCount may be 0 to
 * clear. */
void LcdPreview_UpdateOverlay( const LcdBox_t * pxBoxes, uint32_t ulCount );

/* Cumulative LTDC FIFO-underrun + transfer-error count (polls and clears
 * the flags).  For the media heartbeat. */
uint32_t LcdPreview_Underruns( void );

#endif /* LCD_PREVIEW_H */
