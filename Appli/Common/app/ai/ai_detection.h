/*
 * ai_detection.h — NPU object detection running alongside KVS WebRTC
 * streaming (DCMIPP Pipe2 -> ATON NPU -> IOTCONNECT telemetry).
 *
 * Entire feature is compiled out unless ENABLE_AI_DETECTION is defined.
 * See docs/ai_streaming_integration_plan.md.
 */
#ifndef AI_DETECTION_H
#define AI_DETECTION_H

#include <stdint.h>

#include "lcd_preview.h"   /* LcdBox_t for AiDetection_GetBoxes */

#if defined( ENABLE_AI_DETECTION )

/* One-time init: NPU clocks/RAMs/cache/RIF + inference task creation.
 * Call after HW init, before the camera pipeline starts. */
void AiDetection_Init( void );

/* Configure DCMIPP PIPE2 for the model input (call from media_cam init,
 * after the PIPE1 config, with the sensor dimensions). */
void AiDetection_PipeInit( int lSensorWidth, int lSensorHeight );

/* Start/stop PIPE2 capture (call next to the PIPE1 start/stop). */
void AiDetection_PipeStart( void );
void AiDetection_PipeStop( void );

/* Submit a completed PIPE1 NV12 frame (Y plane + interleaved UV) as the NN
 * source.  The DCMIPP PIPE2 shared fork is dead on this silicon (delivers
 * only 0xFF), so the NPU is fed by CPU-downscaling the working PIPE1 frame
 * to the model input.  Call from the media task after each frame; the call
 * self-rate-limits and no-ops while the NPU is busy. */
void AiDetection_SubmitNV12( const uint8_t * pucY, const uint8_t * pucUV,
                             uint32_t ulSrcWidth, uint32_t ulSrcHeight );

/* PIPE2 vsync/frame-done hook — call from the DCMIPP PIPE2 frame event
 * callback. Rotates capture buffers and wakes the inference task. */
void AiDetection_FrameDoneISR( void );

/* Telemetry snapshot (Phase 3): people count, best confidence (percent),
 * last inference duration (ms), total inference count.  Returns 0 until
 * the network is initialised (or when AI is compiled out); any output
 * pointer may be NULL. */
uint8_t AiDetection_GetTelemetry( int32_t * plDetections,
                                  uint32_t * pulTopConfPct,
                                  uint32_t * pulInferMs,
                                  uint32_t * pulInferences );

/* Latest detection boxes (normalized, see LcdBox_t in lcd_preview.h).
 * Copies up to ulMax boxes into pxBoxes; returns the count. */
uint32_t AiDetection_GetBoxes( LcdBox_t * pxBoxes, uint32_t ulMax );

#else /* !ENABLE_AI_DETECTION */

#define AiDetection_Init()                    do {} while( 0 )
#define AiDetection_PipeInit( w, h )          do { ( void ) ( w ); ( void ) ( h ); } while( 0 )
#define AiDetection_PipeStart()               do {} while( 0 )
#define AiDetection_PipeStop()                do {} while( 0 )
#define AiDetection_SubmitNV12( y, uv, w, h )  do { ( void ) ( y ); ( void ) ( uv ); ( void ) ( w ); ( void ) ( h ); } while( 0 )
#define AiDetection_FrameDoneISR()            do {} while( 0 )
#define AiDetection_GetTelemetry( d, c, m, n )    ( ( uint8_t ) 0U )
#define AiDetection_GetBoxes( b, n )              ( ( uint32_t ) 0U )

#endif /* ENABLE_AI_DETECTION */

#endif /* AI_DETECTION_H */
