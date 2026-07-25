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

/* Master switch: 0 = LcdPreview_Init no-ops, every other call no-ops via
 * the ready flag — byte-identical media/network behavior to a build
 * without the LCD.
 * History: with the W6X Wi-Fi transport, an active LCD correlated with
 * instant relay-TX starvation (RF-level interference with the module —
 * the panel itself worked, video was visible).  On the Ethernet
 * transport (NET_USE_ETHERNET, radio compiled out) that mechanism is
 * gone: re-enabled 2026-07-22. */
/* TEST 2026-07-23: LCD OFF for the Wi-Fi (NET_USE_ETHERNET=0) KVS retest.
 * An active LCD panel radiates into the W6X module and starved relay TX
 * (instant rx_stall) on the Wi-Fi transport — that is why the LCD was only
 * re-enabled after the Ethernet pivot.  Keep it off to isolate whether the
 * V1.3.0 Wi-Fi transport itself can sustain KVS; re-enable ( 1 ) once we
 * know the transport holds.
 * 2026-07-25: LCD-ON + UDP relay CONFIRMED CLEAN — relay session held
 * F0->F2160+ (~154s+), ovr=0 lur=0, zero TLS stalls, only 2 brief rx_stall
 * clusters (<=14ms, FEWER than the LCD-off run -> the panel is NOT the
 * cause; those are independent W6X SPI-bus hiccups).  The old EMI-starvation
 * was specific to the fragile TCP-TX TURN path, which the UDP-relay build no
 * longer uses.  LCD ships ON by default on the Wi-Fi UDP-relay build. */
#define LCD_PREVIEW_ENABLE    ( 1 )

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

/* Runtime on/off for the whole preview (video + overlay).  Powerup default is
 * ON.  The camera and AI detection run continuously regardless; this only
 * controls what the panel displays.  Task context. */
void LcdPreview_SetEnabled( uint8_t ucEnable );

/* Redraw the overlay with the given boxes and flip it.  Task context
 * (called from the AI task after each inference).  ulCount may be 0 to
 * clear. */
void LcdPreview_UpdateOverlay( const LcdBox_t * pxBoxes, uint32_t ulCount );

/* Cumulative LTDC FIFO-underrun + transfer-error count (polls and clears
 * the flags).  For the media heartbeat. */
uint32_t LcdPreview_Underruns( void );

#endif /* LCD_PREVIEW_H */
